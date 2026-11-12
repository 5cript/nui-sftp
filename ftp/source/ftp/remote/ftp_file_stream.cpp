#include <ftp/remote/ftp_file_stream.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace FileTransferProtocol
{
    namespace asio = boost::asio;
    using Detail::errorOf;
    using RemoteIo::RemoteErrorKind;

    bool OpenStreams::add(std::shared_ptr<FtpFileStream> stream)
    {
        std::scoped_lock lock{mutex_};
        if (closed_)
            return false;
        streams_.push_back(std::move(stream));
        return true;
    }

    void OpenStreams::remove(FtpFileStream const* stream)
    {
        std::shared_ptr<FtpFileStream> removed{};
        {
            std::scoped_lock lock{mutex_};
            const auto found = std::ranges::find(streams_, stream, &std::shared_ptr<FtpFileStream>::get);
            if (found == streams_.end())
                return;
            removed = std::move(*found);
            streams_.erase(found);
        }
        // The last owner may be this one, the stream goes outside the lock.
    }

    std::vector<std::shared_ptr<FtpFileStream>> OpenStreams::close()
    {
        std::scoped_lock lock{mutex_};
        closed_ = true;
        return std::exchange(streams_, {});
    }

    std::size_t OpenStreams::size() const
    {
        std::scoped_lock lock{mutex_};
        return streams_.size();
    }

    std::shared_ptr<FtpFileStream> FtpFileStream::create(
        FileSystemSession session,
        FtpFileSystemOptions options,
        RemoteIo::ProcessingStrand* strand,
        Lease lease,
        Paths::RemotePath path,
        Direction direction,
        std::optional<std::uint64_t> size,
        std::weak_ptr<OpenStreams> streams
    )
    {
        return std::make_shared<FtpFileStream>(
            Private{},
            std::move(session),
            std::move(options),
            strand,
            std::move(lease),
            std::move(path),
            direction,
            size,
            std::move(streams)
        );
    }

    FtpFileStream::FtpFileStream(
        Private,
        FileSystemSession session,
        FtpFileSystemOptions options,
        RemoteIo::ProcessingStrand* strand,
        Lease lease,
        Paths::RemotePath path,
        Direction direction,
        std::optional<std::uint64_t> size,
        std::weak_ptr<OpenStreams> streams
    )
        : session_{std::move(session)}
        , options_{std::move(options)}
        , strand_{strand}
        , path_{std::move(path)}
        , direction_{direction}
        , size_{size}
        , streams_{std::move(streams)}
        , transfer_{makeTransfer(std::move(lease), session_, options_)}
    {}

    FtpFileStream::~FtpFileStream()
    {
        if (closed_)
            return;
        if (ring_)
            ring_->stop();
        if (context_)
            context_->fail(closedError());
        endTransfer();
    }

    std::expected<void, RemoteIo::RemoteError> FtpFileStream::seekInStrand(std::size_t position)
    {
        if (auto refused = usable(direction_); !refused)
            return refused;
        if (!started_)
        {
            offset_ = position;
            return {};
        }
        if (direction_ == Direction::Upload)
            return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "An upload cannot move once it started"));
        return callOnConnection<void>(seekOn(transfer_, position), options_.transferCallLimits)
            .transform_error(
                [this](Error error)
                {
                    return broke(std::move(error));
                }
            );
    }

    std::expected<RemoteIo::FileInformation, RemoteIo::RemoteError> FtpFileStream::statInStrand()
    {
        if (closed_)
            return std::unexpected(closedError());
        if (connectionIsFree())
        {
            auto asked = callOnConnection<std::optional<RemoteIo::FileInformation>>(
                statOn(transfer_, path_), options_.operationLimits
            );
            if (!asked)
                return std::unexpected(std::move(asked).error());
            if (*asked)
                return std::move(**asked);
        }
        return session_.engine->template call<RemoteIo::FileInformation>(
            [session = session_, options = options_, path = path_](std::shared_ptr<CallScope> const& scope) mutable
            {
                return Detail::onCommandConnection(
                    std::move(session), std::move(options), scope, RemoteIo::Stat{.path = std::move(path)}
                );
            },
            options_.operationLimits
        );
    }

    std::expected<std::size_t, RemoteIo::RemoteError> FtpFileStream::readSomeInStrand(char* buffer, std::size_t bufferSize)
    {
        if (auto refused = usable(Direction::Download); !refused)
            return std::unexpected(std::move(refused).error());
        const auto limit = std::min(bufferSize, options_.lengthLimit);
        if (limit == 0u)
            return 0u;
        started_ = true;
        auto read =
            callOnConnection<std::size_t>(readOn(transfer_, downloadRequest(), limit), options_.transferCallLimits);
        if (!read)
            return std::unexpected(broke(std::move(read).error()));
        std::memcpy(buffer, transfer_->readBuffer.data(), *read);
        return *read;
    }

    std::future<std::expected<std::size_t, RemoteIo::RemoteError>>
    FtpFileStream::readAll(std::function<bool(std::string_view data)> onRead)
    {
        return strand_->pushPromiseTask(
            [self = shared_from_this(), onRead = std::move(onRead)]()
            {
                return self->readAllInStrand(onRead);
            }
        );
    }

    std::expected<void, RemoteIo::RemoteError> FtpFileStream::writeInStrand(std::string_view data)
    {
        if (auto refused = usable(Direction::Upload); !refused)
            return refused;
        if (data.empty())
            return {};
        started_ = true;
        return callOnConnection<void>(writeOn(transfer_, uploadStart(), std::string{data}), options_.transferCallLimits)
            .transform_error(
                [this](Error error)
                {
                    return broke(std::move(error));
                }
            );
    }

    std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>
    FtpFileStream::readAsyncInStrand(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<bool(SignedSizeType)> onRead
    )
    {
        if (auto refused = usable(Direction::Download); !refused)
            return std::unexpected(std::move(refused).error());
        if (bufferSize <= 0)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The buffer of an asynchronous read is empty"));
        auto const& control = transfer_->session.control;
        const auto ring = DeliveryRing::create(session_.engine, control->strand(), options_.ring);
        auto context = makeContext(ring);
        DeliveryRing::Sink sink = [context,
                                      buffer,
                                      size = static_cast<std::size_t>(bufferSize),
                                      onRead = std::move(onRead)](std::span<char const> bytes)
        {
            return deliver(*context, buffer, size, onRead, bytes);
        };
        const auto spawned = session_.engine->spawn(runAsync(
            weak_from_this(),
            session_.engine,
            control,
            context,
            runDownloadRing(transfer_, ring, context, downloadRequest(), std::move(sink)),
            totalFileSize
        ));
        if (!spawned)
            return std::unexpected(Detail::sessionEndedError());
        started_ = true;
        ring_ = ring;
        context_ = context;
        return context;
    }

    std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>
    FtpFileStream::writeAsyncInStrand(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<SignedSizeType(SignedSizeType)> doRead
    )
    {
        if (auto refused = usable(Direction::Upload); !refused)
            return std::unexpected(std::move(refused).error());
        if (bufferSize <= 0)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The buffer of an asynchronous write is empty"));
        auto const& control = transfer_->session.control;
        const auto ring = SupplyRing::create(session_.engine, control->strand(), options_.ring);
        auto context = makeContext(ring);
        SupplyRing::Source source = [context,
                                        buffer,
                                        bufferSize,
                                        doRead = std::move(doRead),
                                        remaining = totalFileSize](std::span<char> slot) mutable
        {
            return fill(*context, buffer, bufferSize, doRead, remaining, slot);
        };
        const auto spawned = session_.engine->spawn(runAsync(
            weak_from_this(),
            session_.engine,
            control,
            context,
            runUploadRing(transfer_, ring, context, uploadStart(), std::move(source)),
            std::nullopt
        ));
        if (!spawned)
            return std::unexpected(Detail::sessionEndedError());
        started_ = true;
        ring_ = ring;
        context_ = context;
        return context;
    }

    std::expected<void, RemoteIo::RemoteError> FtpFileStream::finishInStrand()
    {
        if (finished_)
            return *finished_;
        if (closed_)
            return std::unexpected(closedError());
        if (broken_)
            return std::unexpected(*broken_);
        // The asynchronous transfer finishes itself, a finish beside it would end the data under its ring.
        if (ring_)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "An asynchronous transfer of the stream runs"));
        if (direction_ == Direction::Download)
        {
            if (!started_)
                finished_.emplace();
            else
                finished_ = callOnConnection<void>(finishDownloadOn(transfer_), options_.transferCallLimits);
        }
        else
        {
            started_ = true;
            finished_ =
                callOnConnection<void>(finishUploadOn(transfer_, uploadStart()), options_.transferCallLimits);
        }
        return *finished_;
    }

    std::size_t FtpFileStream::readLengthLimit() const
    {
        return options_.lengthLimit;
    }

    std::size_t FtpFileStream::writeLengthLimit() const
    {
        return options_.lengthLimit;
    }

    void FtpFileStream::close()
    {
        if (strand_->withinProcessingThread())
            return closeInStrand();
        // A strand that refuses the task was finalized, its close ended the streams already.
        strand_->pushTask(
            [self = shared_from_this()]()
            {
                self->closeInStrand();
            }
        );
    }

    void FtpFileStream::closeInStrand()
    {
        if (closed_)
            return;
        // Leaving the open streams may drop the last owner.
        const auto self = shared_from_this();
        closed_ = true;
        // No delivery or fill reaches the caller's side after this, also from inside one.
        if (ring_)
            ring_->close();
        if (context_)
            context_->fail(closedError());
        endTransfer();
        if (const auto streams = streams_.lock())
            streams->remove(this);
    }

    RemoteIo::ProcessingStrand* FtpFileStream::strand() const
    {
        return strand_;
    }

    asio::awaitable<FtpFileStream::Result<std::optional<RemoteIo::FileInformation>>>
    FtpFileStream::statOn(std::shared_ptr<Transfer> transfer, Paths::RemotePath path)
    {
        if (transfer->session.control->state() != ControlState::Open)
            co_return std::nullopt;
        co_return (co_await runOperation(transfer->metadata(), RemoteIo::Stat{.path = std::move(path)}))
            .transform(
                [](RemoteIo::FileInformation information)
                {
                    return std::optional<RemoteIo::FileInformation>{std::move(information)};
                }
            );
    }

    asio::awaitable<FtpFileStream::Result<std::size_t>>
    FtpFileStream::readOn(std::shared_ptr<Transfer> transfer, DownloadRequest request, std::size_t limit)
    {
        if (!transfer->download)
        {
            auto started = co_await Download::start(transfer->session, std::move(request), transfer->options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            transfer->download = *std::move(started);
        }
        if (transfer->readBuffer.size() < limit)
            transfer->readBuffer.resize(limit);
        co_return co_await transfer->download->readSome(asio::buffer(transfer->readBuffer.data(), limit));
    }

    asio::awaitable<FtpFileStream::Result<void>>
    FtpFileStream::seekOn(std::shared_ptr<Transfer> transfer, std::uint64_t position)
    {
        if (!transfer->download)
            co_return std::unexpected(errorOf(RemoteErrorKind::Failure, "The download never started"));
        co_return transfer->download->seek(position);
    }

    asio::awaitable<FtpFileStream::Result<void>>
    FtpFileStream::startUploadOn(std::shared_ptr<Transfer> transfer, UploadStart start)
    {
        if (transfer->upload)
            co_return Result<void>{};
        if (start.appendsAt)
        {
            // APPE writes at the end of the file, wherever the seek pointed.
            const auto size = co_await sizeOf(transfer->metadata(), start.request.path);
            if (size && *size != *start.appendsAt)
            {
                co_return std::unexpected(errorOf(
                    RemoteErrorKind::Unsupported,
                    fmt::format(
                        "The server has no REST STREAM and the file ends at {}, not at {}", *size, *start.appendsAt
                    )
                ));
            }
            if (!size && size.error().kind != RemoteErrorKind::Unsupported)
                co_return std::unexpected(size.error());
        }
        auto started = co_await Upload::start(transfer->session, std::move(start.request), transfer->options);
        if (!started)
            co_return std::unexpected(std::move(started).error());
        transfer->upload = *std::move(started);
        co_return Result<void>{};
    }

    asio::awaitable<FtpFileStream::Result<void>>
    FtpFileStream::writeOn(std::shared_ptr<Transfer> transfer, UploadStart start, std::string data)
    {
        if (auto started = co_await startUploadOn(transfer, std::move(start)); !started)
            co_return started;
        co_return co_await transfer->upload->write(asio::buffer(data));
    }

    asio::awaitable<FtpFileStream::Result<void>> FtpFileStream::finishDownloadOn(std::shared_ptr<Transfer> transfer)
    {
        if (!transfer->download)
            co_return Result<void>{};
        co_return (co_await transfer->download->finish()).transform([](TransferResult const&) {});
    }

    asio::awaitable<FtpFileStream::Result<void>>
    FtpFileStream::finishUploadOn(std::shared_ptr<Transfer> transfer, UploadStart start)
    {
        // An upload that wrote nothing still stores its empty file.
        if (auto started = co_await startUploadOn(transfer, std::move(start)); !started)
            co_return started;
        co_return (co_await transfer->upload->finish()).transform([](UploadResult const&) {});
    }

    bool FtpFileStream::connectionIsFree() const
    {
        return !started_ || (finished_ && finished_->has_value());
    }

    FtpFileStream::Result<void> FtpFileStream::usable(Direction direction) const
    {
        if (closed_)
            return std::unexpected(closedError());
        if (finished_)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The transfer of the stream finished"));
        if (broken_)
            return std::unexpected(*broken_);
        if (ring_)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "An asynchronous transfer of the stream runs"));
        if (direction == direction_)
            return {};
        if (direction_ == Direction::Download)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The stream was opened for reading"));
        return std::unexpected(errorOf(RemoteErrorKind::Failure, "The stream was opened for writing"));
    }

    FtpFileStream::Error FtpFileStream::broke(Error error)
    {
        broken_ = error;
        return error;
    }

    DownloadRequest FtpFileStream::downloadRequest() const
    {
        return DownloadRequest{.path = path_.value(), .offset = offset_, .expectedSize = size_};
    }

    FtpFileStream::UploadStart FtpFileStream::uploadStart() const
    {
        if (offset_ == 0u)
            return UploadStart{.request = {.path = path_.value()}};
        if (transfer_->session.restartStream)
            return UploadStart{.request = {.path = path_.value(), .mode = UploadMode::Store, .offset = offset_}};
        return UploadStart{.request = {.path = path_.value(), .mode = UploadMode::Append}, .appendsAt = offset_};
    }

    FtpFileStream::Result<std::size_t>
    FtpFileStream::readAllInStrand(std::function<bool(std::string_view data)> const& onRead)
    {
        std::vector<char> buffer(options_.lengthLimit);
        std::size_t total = 0u;
        auto read = readSomeInStrand(buffer.data(), buffer.size());
        while (read && *read > 0u)
        {
            total += *read;
            // Stopped early, the close ends the transfer.
            if (!onRead(std::string_view{buffer.data(), *read}))
                return total;
            read = readSomeInStrand(buffer.data(), buffer.size());
        }
        if (!read)
            return std::unexpected(std::move(read).error());
        return finishInStrand().transform(
            [total]()
            {
                return total;
            }
        );
    }

    MetadataSession FtpFileStream::Transfer::metadata() const
    {
        return MetadataSession{.transfer = session, .dialect = dialect, .options = options, .limits = listing};
    }

    std::shared_ptr<FtpFileStream::Transfer>
    FtpFileStream::makeTransfer(Lease lease, FileSystemSession const& session, FtpFileSystemOptions const& options)
    {
        auto transfer = std::make_shared<Transfer>();
        transfer->session = lease.transferSession();
        transfer->options = options.transfer;
        transfer->dialect = session.dialect;
        transfer->listing = options.listing;
        transfer->lease = std::move(lease);
        return transfer;
    }

    void FtpFileStream::endTransfer()
    {
        // The last holder gives the connection back: the ABOR below, or a coroutine of the stream that still runs on
        // the connection, a transfer that was starting included. Nothing else may send commands there before.
        const auto transfer = std::exchange(transfer_, nullptr);
        if (!transfer)
            return;
        const auto finishedWell = finished_ && finished_->has_value();
        if (!started_ || finishedWell)
            return;
        session_.engine->postTo(
            transfer->session.control->strand(),
            [transfer]()
            {
                abortTransfer(*transfer);
            }
        );
    }

    void FtpFileStream::AsyncLinks::wake() const
    {
        const auto lockedRing = ring.lock();
        if (!lockedRing)
            return;
        if (const auto lockedContext = context.lock(); lockedContext && !lockedContext->hasEnded())
        {
            lockedRing->wake();
            return;
        }
        lockedRing->stop();
        const auto lockedTransfer = transfer.lock();
        const auto lockedEngine = engine.lock();
        if (!lockedTransfer || !lockedEngine)
            return;
        lockedEngine->postTo(
            lockedTransfer->session.control->strand(),
            [lockedTransfer]()
            {
                abortTransfer(*lockedTransfer);
            }
        );
    }

    std::shared_ptr<FtpFileStream::Context> FtpFileStream::makeContext(std::shared_ptr<Ring> const& ring) const
    {
        const auto links = std::make_shared<AsyncLinks>(AsyncLinks{
            .ring = ring,
            .transfer = transfer_,
            .engine = session_.engine,
        });
        auto context = std::make_shared<Context>(
            [links]()
            {
                links->wake();
            }
        );
        links->context = context;
        return context;
    }

    bool FtpFileStream::deliver(
        Context& context,
        char* buffer,
        std::size_t bufferSize,
        std::function<bool(SignedSizeType)> const& onRead,
        std::span<char const> bytes
    )
    {
        for (std::size_t offset = 0u; offset < bytes.size(); offset += bufferSize)
        {
            // A close or cancel from inside onRead ends the context.
            if (context.hasEnded())
                return false;
            const auto piece = std::min(bufferSize, bytes.size() - offset);
            std::memcpy(buffer, bytes.data() + offset, piece);
            if (!onRead(static_cast<SignedSizeType>(piece)))
            {
                context.fail(RemoteIo::RemoteError{
                    .kind = RemoteErrorKind::Failure,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message = "Storing the received data failed",
                    .wrapperError = RemoteIo::WrapperErrors::LocalIoFailure,
                });
                return false;
            }
            context.addBytesTransferred(static_cast<SignedSizeType>(piece));
        }
        context.calculateBytesPerSecond();
        context.reportProgress();
        return !context.hasEnded();
    }

    FtpFileStream::Result<std::size_t> FtpFileStream::fill(
        Context& context,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<SignedSizeType(SignedSizeType)> const& doRead,
        SignedSizeType& remaining,
        std::span<char> slot
    )
    {
        if (context.hasEnded())
            return std::unexpected(errorOf(RemoteErrorKind::Cancelled, "The transfer ended"));
        std::size_t filled = 0u;
        while (filled < slot.size() && remaining > 0)
        {
            const auto wanted =
                std::min({bufferSize, static_cast<SignedSizeType>(slot.size() - filled), remaining});
            const auto got = doRead(wanted);
            if (got <= 0 || got > wanted)
            {
                return std::unexpected(RemoteIo::RemoteError{
                    .kind = RemoteErrorKind::Failure,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message = "Reading the data to send failed",
                    .wrapperError = RemoteIo::WrapperErrors::LocalIoFailure,
                });
            }
            std::memcpy(slot.data() + filled, buffer, static_cast<std::size_t>(got));
            filled += static_cast<std::size_t>(got);
            remaining -= got;
            context.addBytesTransferred(got);
        }
        context.calculateBytesPerSecond();
        context.reportProgress();
        return filled;
    }

    asio::awaitable<FtpFileStream::Result<void>> FtpFileStream::runDownloadRing(
        std::shared_ptr<Transfer> transfer,
        std::shared_ptr<DeliveryRing> ring,
        std::shared_ptr<Context> context,
        DownloadRequest request,
        DeliveryRing::Sink sink
    )
    {
        if (!transfer->download)
        {
            auto started = co_await Download::start(transfer->session, std::move(request), transfer->options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            transfer->download = *std::move(started);
        }
        const auto ran = co_await ring->run(
            [transfer](asio::mutable_buffer buffer)
            {
                return transfer->download->readSome(buffer);
            },
            std::move(sink),
            [context]()
            {
                return context->paused();
            }
        );
        if (!ran)
        {
            transfer->download->abort();
            co_return ran;
        }
        co_return (co_await transfer->download->finish()).transform([](TransferResult const&) {});
    }

    asio::awaitable<FtpFileStream::Result<void>> FtpFileStream::runUploadRing(
        std::shared_ptr<Transfer> transfer,
        std::shared_ptr<SupplyRing> ring,
        std::shared_ptr<Context> context,
        UploadStart start,
        SupplyRing::Source source
    )
    {
        if (auto started = co_await startUploadOn(transfer, std::move(start)); !started)
            co_return started;
        const auto ran = co_await ring->run(
            std::move(source),
            [transfer](asio::const_buffer data)
            {
                return transfer->upload->write(data);
            },
            [context]()
            {
                return context->paused();
            }
        );
        if (!ran)
        {
            transfer->upload->abort();
            co_return ran;
        }
        co_return (co_await transfer->upload->finish()).transform([](UploadResult const&) {});
    }

    asio::awaitable<void> FtpFileStream::runAsync(
        std::weak_ptr<FtpFileStream> stream,
        std::shared_ptr<Engine> engine,
        std::shared_ptr<ControlConnection> control,
        std::shared_ptr<Context> context,
        asio::awaitable<Result<void>> ringSide,
        std::optional<SignedSizeType> expectedBytes
    )
    {
        auto [exception, outcome] = co_await asio::co_spawn(
            control->strand(),
            Detail::withoutThrowing<void>(std::move(ringSide)),
            asio::as_tuple(asio::use_awaitable)
        );
        // Fails the session, see Engine::spawn().
        if (exception)
            std::rethrow_exception(exception);
        auto end = [stream, context, outcome = std::move(outcome), expectedBytes]() mutable
        {
            if (outcome && expectedBytes && context->bytesTransferred() < *expectedBytes)
            {
                outcome = std::unexpected(RemoteIo::RemoteError{
                    .kind = RemoteErrorKind::Truncated,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message = "The remote file ended before the expected size was read",
                    .wrapperError = RemoteIo::WrapperErrors::UnexpectedEndOfFile,
                });
            }
            if (const auto locked = stream.lock())
                locked->asyncEnded(outcome);
            context->calculateBytesPerSecond();
            if (outcome)
                context->complete();
            else
                context->fail(std::move(outcome).error());
        };
        // Behind every delivery of the ring, which pushed them before its run ended.
        if (!engine->pushTask(end))
            end();
    }

    void FtpFileStream::asyncEnded(Result<void> outcome)
    {
        ring_.reset();
        if (!finished_)
            finished_ = std::move(outcome);
    }

    void FtpFileStream::abortTransfer(Transfer& transfer)
    {
        if (transfer.download)
            transfer.download->abort();
        if (transfer.upload)
            transfer.upload->abort();
    }

    FtpFileStream::Error FtpFileStream::closedError()
    {
        return errorOf(RemoteErrorKind::ConnectionLost, "The FTP file stream was closed");
    }
}
