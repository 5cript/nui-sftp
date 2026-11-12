#include <ftp/remote/ftp_file_stream.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/buffer.hpp>

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
        , transfer_{std::make_shared<Transfer>(Transfer{
              .session = lease.transferSession(),
              .options = options_.transfer,
          })}
        , lease_{std::move(lease)}
    {}

    FtpFileStream::~FtpFileStream()
    {
        if (!closed_)
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
        return callOnConnection<void>(seekOn(transfer_, position))
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
        auto read = callOnConnection<std::size_t>(readOn(transfer_, downloadRequest(), limit));
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
        return callOnConnection<void>(writeOn(transfer_, uploadRequest(), std::string{data}))
            .transform_error(
                [this](Error error)
                {
                    return broke(std::move(error));
                }
            );
    }

    std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>
    FtpFileStream::readAsyncInStrand(SignedSizeType, char*, SignedSizeType, std::function<bool(SignedSizeType)>)
    {
        return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP has no asynchronous reads yet"));
    }

    std::expected<std::shared_ptr<RemoteIo::AsyncTransferContext>, RemoteIo::RemoteError>
    FtpFileStream::writeAsyncInStrand(SignedSizeType, char*, SignedSizeType, std::function<SignedSizeType(SignedSizeType)>)
    {
        return std::unexpected(errorOf(RemoteErrorKind::Unsupported, "FTP has no asynchronous writes yet"));
    }

    std::expected<void, RemoteIo::RemoteError> FtpFileStream::finishInStrand()
    {
        if (finished_)
            return *finished_;
        if (closed_)
            return std::unexpected(closedError());
        if (broken_)
            return std::unexpected(*broken_);
        if (direction_ == Direction::Download)
        {
            if (!started_)
                finished_.emplace();
            else
                finished_ = callOnConnection<void>(finishDownloadOn(transfer_));
        }
        else
        {
            started_ = true;
            finished_ = callOnConnection<void>(finishUploadOn(transfer_, uploadRequest()));
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
        endTransfer();
        if (const auto streams = streams_.lock())
            streams->remove(this);
    }

    RemoteIo::ProcessingStrand* FtpFileStream::strand() const
    {
        return strand_;
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
    FtpFileStream::writeOn(std::shared_ptr<Transfer> transfer, UploadRequest request, std::string data)
    {
        if (!transfer->upload)
        {
            auto started = co_await Upload::start(transfer->session, std::move(request), transfer->options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            transfer->upload = *std::move(started);
        }
        co_return co_await transfer->upload->write(asio::buffer(data));
    }

    asio::awaitable<FtpFileStream::Result<void>> FtpFileStream::finishDownloadOn(std::shared_ptr<Transfer> transfer)
    {
        if (!transfer->download)
            co_return Result<void>{};
        co_return (co_await transfer->download->finish()).transform([](TransferResult const&) {});
    }

    asio::awaitable<FtpFileStream::Result<void>>
    FtpFileStream::finishUploadOn(std::shared_ptr<Transfer> transfer, UploadRequest request)
    {
        // An upload that wrote nothing still stores its empty file.
        if (!transfer->upload)
        {
            auto started = co_await Upload::start(transfer->session, std::move(request), transfer->options);
            if (!started)
                co_return std::unexpected(std::move(started).error());
            transfer->upload = *std::move(started);
        }
        co_return (co_await transfer->upload->finish()).transform([](UploadResult const&) {});
    }

    FtpFileStream::Result<void> FtpFileStream::usable(Direction direction) const
    {
        if (closed_)
            return std::unexpected(closedError());
        if (finished_)
            return std::unexpected(errorOf(RemoteErrorKind::Failure, "The transfer of the stream finished"));
        if (broken_)
            return std::unexpected(*broken_);
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

    UploadRequest FtpFileStream::uploadRequest() const
    {
        if (offset_ == 0u)
            return UploadRequest{.path = path_.value()};
        if (transfer_->session.restartStream)
            return UploadRequest{.path = path_.value(), .mode = UploadMode::Store, .offset = offset_};
        return UploadRequest{.path = path_.value(), .mode = UploadMode::Append};
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

    void FtpFileStream::endTransfer()
    {
        const auto finishedWell = finished_ && finished_->has_value();
        if (started_ && !finishedWell)
        {
            session_.engine->postTo(
                transfer_->session.control->strand(),
                [transfer = transfer_]()
                {
                    if (transfer->download)
                        transfer->download->abort();
                    if (transfer->upload)
                        transfer->upload->abort();
                }
            );
        }
        // Behind the ABOR on the connection's strand: the pool asks there whether the connection is still open.
        lease_.release();
    }

    FtpFileStream::Error FtpFileStream::closedError()
    {
        return errorOf(RemoteErrorKind::ConnectionLost, "The FTP file stream was closed");
    }
}
