#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>
#include <ssh/async/request_window.hpp>

#include <algorithm>
#include <cassert>
#include <deque>
#include <utility>
#include <chrono>

namespace SecureShell
{
    namespace
    {
        using TaskStatus = ProcessingThread::PermanentTaskStatus;

        /** @brief Bytes one upload moves per turn at most, and a download at least; several turns make up a cycle. */
        constexpr IFileStream::SignedSizeType targetBytesPerTurn = 256 * 1024;

        /** @brief Size of a single sftp write: the session's preferred chunk, bounded by the
         *         leased buffer and the server limit. A call cannot be interrupted, so the
         *         session sizes it to take about one target call duration; the turn and the cycle
         *         (after which remaining transfers skip, with the thread rotating who goes first)
         *         are bounded by that same duration.
         *
         *  @param sftp        Session owning the chunk controller.
         *  @param bufferSize  Leased buffer size.
         *  @param lengthLimit Server-negotiated maximum per call, 0 if unknown.
         */
        IFileStream::SignedSizeType transferChunkBytes(
            SftpSession const& sftp,
            IFileStream::SignedSizeType bufferSize,
            IFileStream::SignedSizeType lengthLimit
        )
        {
            using SignedSizeType = IFileStream::SignedSizeType;
            const auto maxChunk =
                std::max(SignedSizeType{1}, lengthLimit > 0 ? std::min(bufferSize, lengthLimit) : bufferSize);
            return std::min(maxChunk, static_cast<SignedSizeType>(sftp.chunkController().preferredTransferChunk(maxChunk)));
        }

        bool turnExpired(std::chrono::steady_clock::time_point turnStart, std::chrono::steady_clock::duration limit)
        {
            return std::chrono::steady_clock::now() - turnStart > limit;
        }

        SftpError expiredError(std::string_view what)
        {
            return SftpError{
                .message = fmt::format("{} expired during the transfer", what),
                .wrapperError = WrapperErrors::SharedPtrDestroyed,
            };
        }

        SftpError transferError(SftpSession const& sftp, std::string_view what)
        {
            auto error = sftp.lastError();
            error.message = std::string{what};
            return error;
        }

        /**
         * @brief A read request that was sent but whose data was not taken yet.
         */
        struct PendingRead
        {
            std::unique_ptr<sftp_aio_struct, decltype(&sftp_aio_free)> request;
            std::uint64_t offset;
            IFileStream::SignedSizeType length;
        };

        /**
         * @brief What one read request brought.
         */
        struct ReceivedRead
        {
            std::uint64_t offset;
            IFileStream::SignedSizeType requested;
            IFileStream::SignedSizeType received;
        };

        /**
         * @brief The read requests one download keeps in flight, oldest first.
         *
         * A blocking sftp_read sends one request and waits for its answer, so the link idles for a
         * round trip and the server's processing per call. Keeping a window of requests in flight
         * hides both. Answers to requests nobody waits for would stay queued in the sftp session,
         * so the pipeline is drained before a transfer ends.
         */
        class ReadPipeline
        {
          public:
            using SignedSizeType = IFileStream::SignedSizeType;

            bool empty() const noexcept
            {
                return pending_.empty();
            }

            SignedSizeType inFlightBytes() const noexcept
            {
                return inFlightBytes_;
            }

            RequestWindow& window() noexcept
            {
                return window_;
            }

            /**
             * @brief Sends requests until the window is full or everything is requested.
             *
             * @param unrequested Bytes of the file no request asks for yet.
             * @param upperBound Largest request, the buffer size or the server limit.
             */
            std::expected<void, SftpError>
            fill(SftpSession const& sftp, sftp_file file, SignedSizeType unrequested, SignedSizeType upperBound)
            {
                while (unrequested > 0 && inFlightBytes_ < window_.windowBytes())
                {
                    const auto length = std::min(unrequested, window_.requestBytes(upperBound));
                    const auto offset = sftp_tell64(file);
                    sftp_aio request{nullptr};
                    const auto sent = sftp_aio_begin_read(file, static_cast<std::size_t>(length), &request);
                    if (sent < 0)
                        return std::unexpected(transferError(sftp, "Requesting from the remote file failed"));
                    pending_.push_back(
                        PendingRead{
                            .request = {request, sftp_aio_free},
                            .offset = offset,
                            .length = static_cast<SignedSizeType>(sent),
                        }
                    );
                    inFlightBytes_ += sent;
                    unrequested -= sent;
                }
                return {};
            }

            /**
             * @brief Waits for the oldest request and copies its data into the buffer.
             */
            std::expected<ReceivedRead, SftpError>
            receiveOldest(SftpSession const& sftp, char* buffer, SignedSizeType bufferSize)
            {
                auto oldest = std::move(pending_.front());
                pending_.pop_front();
                inFlightBytes_ -= oldest.length;

                auto* request = oldest.request.release();
                const auto received = sftp_aio_wait_read(&request, buffer, static_cast<std::size_t>(bufferSize));
                // Frees the request in every case but SSH_AGAIN, which a blocking file does not return.
                oldest.request.reset(request);
                if (received < 0)
                    return std::unexpected(transferError(sftp, "Reading the remote file failed"));
                return ReceivedRead{
                    .offset = oldest.offset,
                    .requested = oldest.length,
                    .received = static_cast<SignedSizeType>(received),
                };
            }

            /**
             * @brief Waits for every request still in flight and throws its data away.
             *
             * @param file The open remote file, null when it was closed already, in which case
             *        the requests are only freed.
             */
            void discard(sftp_file file, char* buffer, SignedSizeType bufferSize) noexcept
            {
                if (file != nullptr)
                {
                    for (auto& pending : pending_)
                    {
                        auto* request = pending.request.release();
                        std::ignore = sftp_aio_wait_read(&request, buffer, static_cast<std::size_t>(bufferSize));
                        pending.request.reset(request);
                    }
                }
                pending_.clear();
                inFlightBytes_ = 0;
            }

          private:
            std::deque<PendingRead> pending_{};
            SignedSizeType inFlightBytes_{0};
            RequestWindow window_{};
        };
    }

#define VERIFY_FILE_STREAM() \
    if (!file_) \
    return std::unexpected(SftpError{.message = "File is null", .wrapperError = WrapperErrors::FileNull})

    template <typename FunctionT>
    void FileStream::perform(FunctionT&& func)
    {
        if (auto sftp = sftp_.lock(); sftp)
            sftp->perform(std::forward<FunctionT>(func));
    }

    template <typename FunctionT>
    auto FileStream::performPromise(FunctionT&& func)
    {
        if (auto sftp = sftp_.lock(); sftp)
            return sftp->performPromise(std::forward<FunctionT>(func));

        using ResultType = std::invoke_result_t<std::decay_t<FunctionT>>;
        std::promise<ResultType> promise{};
        promise.set_value(
            std::unexpected(
                SftpError{
                    .message = "Owner is null",
                    .wrapperError = WrapperErrors::OwnerNull,
                }
            )
        );
        return promise.get_future();
    }

    FileStream::FileStream(std::shared_ptr<SftpSession> sftp, sftp_file file, sftp_limits_struct limits)
        : sftp_{std::move(sftp)}
        , file_{file, makeFileDeleter()}
        , limits_{limits}
    {}
    FileStream::~FileStream() = default;
    FileStream::FileStream(FileStream&& other)
        : sftp_{std::move(other.sftp_)}
        , file_{other.file_.release(), makeFileDeleter()}
        , limits_{other.limits_}
    {}
    void FileStream::close(bool isBackElement)
    {
        if (auto sftp = sftp_.lock(); sftp)
        {
            if (!sftp->strand_->withinProcessingThread())
            {
                (void)
                    sftp->performPromise(
                            [this, isBackElement, sftp]() -> bool
                            {
                                closeInStrand(isBackElement);
                                return true;
                            }
                    ).get();
            }
            else
            {
                closeInStrand(isBackElement);
            }
        }
    }
    void FileStream::closeInStrand(bool isBackElement)
    {
        auto sftp = sftp_.lock();
        if (!sftp)
            return;
        assert(sftp->strand_->withinProcessingThread());
        file_.reset();
        sftp->fileStreamRemoveItself(this, isBackElement);
    }
    std::function<void(sftp_file)> FileStream::makeFileDeleter()
    {
        return [this](sftp_file file)
        {
            if (auto sftp = this->sftp_.lock(); sftp)
            {
                // This is safe because file is just a pointer, even if 'this' is deleted by this point.
                sftp->perform(
                    [file]()
                    {
                        sftp_close(file);
                    }
                );
            }
        };
    }
    FileStream& FileStream::operator=(FileStream&& other)
    {
        if (this != &other)
        {
            sftp_ = std::move(other.sftp_);
            file_ = std::unique_ptr<sftp_file_struct, std::function<void(sftp_file)>>{
                other.file_.release(), makeFileDeleter()
            };
        }
        return *this;
    }
    std::expected<void, SftpError> FileStream::seekInStrand(std::size_t pos)
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        sftp_seek64(file_.get(), pos);
        return {};
    }
    std::future<std::expected<void, SftpError>> FileStream::seek(std::size_t pos)
    {
        return performPromise(
            [this, pos]()
            {
                return seekInStrand(pos);
            }
        );
    }
    std::expected<FileInformation, SftpError> FileStream::statInStrand()
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)> attributes{
            sftp_fstat(file_.get()), sftp_attributes_free
        };
        if (attributes == nullptr)
            return std::unexpected(lastError());
        return fromSftpAttributes(attributes.get());
    }
    std::future<std::expected<FileInformation, SftpError>> FileStream::stat()
    {
        return performPromise(
            [this]()
            {
                return statInStrand();
            }
        );
    }
    std::expected<std::size_t, SftpError> FileStream::tellInStrand()
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        return static_cast<std::size_t>(sftp_tell64(file_.get()));
    }
    std::future<std::expected<std::size_t, SftpError>> FileStream::tell()
    {
        return performPromise(
            [this]()
            {
                return tellInStrand();
            }
        );
    }
    std::expected<void, SftpError> FileStream::rewindInStrand()
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        sftp_rewind(file_.get());
        return {};
    }
    std::future<std::expected<void, SftpError>> FileStream::rewind()
    {
        return performPromise(
            [this]()
            {
                return rewindInStrand();
            }
        );
    }
    SftpError FileStream::lastError() const
    {
        if (auto sftp = sftp_.lock(); sftp)
        {
            return sftp->lastError();
        }
        else
        {
            return SftpError{
                .message = "shared_ptr expired",
                .wrapperError = WrapperErrors::SharedPtrDestroyed,
            };
        }
    }
    std::expected<std::size_t, SftpError> FileStream::readSomeInStrand(char* buffer, std::size_t bufferSize)
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        const auto result = sftp_read(file_.get(), buffer, bufferSize);
        if (result < 0)
            return std::unexpected(lastError());
        return static_cast<std::size_t>(result);
    }
    std::future<std::expected<std::size_t, SftpError>> FileStream::readSome(char* buffer, std::size_t bufferSize)
    {
        return performPromise(
            [this, buffer, bufferSize]()
            {
                return readSomeInStrand(buffer, bufferSize);
            }
        );
    }

    std::future<std::expected<std::size_t, SftpError>>
    FileStream::readAll(std::function<bool(std::string_view data)> onReadFunc)
    {
        struct ReadState : public std::enable_shared_from_this<ReadState>
        {
            FileStream& stream;
            BufferLease buffer;
            std::function<bool(std::string_view data)> callback;
            std::promise<std::expected<std::size_t, SftpError>> promise;
            std::size_t totalRead;

            ReadState(FileStream& stream, BufferLease buffer, std::function<bool(std::string_view data)> callback)
                : stream{stream}
                , buffer{std::move(buffer)}
                , callback{std::move(callback)}
                , promise{}
                , totalRead{0}
            {}

            void onRead(std::make_signed_t<std::size_t> amount)
            {
                // The callback runs before the promise is set: the waiting caller may destroy what it captured.
                if (amount < 0)
                {
                    const auto error = stream.lastError();
                    callback({});
                    promise.set_value(std::unexpected(error));
                    return;
                }

                if (amount == 0)
                {
                    callback({});
                    promise.set_value(totalRead);
                    return;
                }

                totalRead += amount;

                if (!callback({buffer.data(), static_cast<std::size_t>(amount)}))
                {
                    promise.set_value(totalRead);
                    return;
                }

                doRead();
            }

            void doRead()
            {
                stream.perform(
                    [state = shared_from_this()]()
                    {
                        if (!state->stream.file_)
                        {
                            state->promise.set_value(
                                std::unexpected(
                                    SftpError{
                                        .message = "File is null",
                                        .wrapperError = WrapperErrors::FileNull,
                                    }
                                )
                            );
                            return;
                        }
                        state->onRead(sftp_read(state->stream.file_.get(), state->buffer.data(), state->buffer.size()));
                    }
                );
            }
        };

        auto sftp = sftp_.lock();
        if (!sftp)
        {
            std::promise<std::expected<std::size_t, SftpError>> promise{};
            promise.set_value(
                std::unexpected(
                    SftpError{
                        .message = "Owner is null",
                        .wrapperError = WrapperErrors::OwnerNull,
                    }
                )
            );
            return promise.get_future();
        }

        auto lease = sftp->bufferProvider().leaseForTransfer(readLengthLimit(), readLengthLimit());
        if (lease.empty())
        {
            std::promise<std::expected<std::size_t, SftpError>> promise{};
            promise.set_value(
                std::unexpected(
                    SftpError{
                        .message = "No buffer available from pool",
                        .wrapperError = WrapperErrors::BufferUnavailable,
                    }
                )
            );
            return promise.get_future();
        }

        auto state = std::make_shared<ReadState>(*this, std::move(lease), std::move(onReadFunc));
        state->doRead();
        return state->promise.get_future();
    }
    std::size_t FileStream::writeLengthLimit() const
    {
        return limits_.max_write_length;
    }
    std::size_t FileStream::readLengthLimit() const
    {
        return limits_.max_read_length;
    }
    void FileStream::writePart(
        std::string_view toWrite,
        std::function<void(std::expected<void, SftpError>&&)> onWriteComplete
    )
    {
        perform(
            [this, toWrite, onWriteComplete = std::move(onWriteComplete)]()
            {
                if (!file_)
                    return;

                const auto written =
                    sftp_write(file_.get(), toWrite.data(), std::min(toWrite.size(), writeLengthLimit()));

                if (written < 0)
                    return onWriteComplete(std::unexpected(lastError()));

                if (static_cast<std::size_t>(written) == toWrite.size())
                    return onWriteComplete({});

                if (written == 0 && !toWrite.empty())
                {
                    return onWriteComplete(
                        std::unexpected(
                            SftpError{
                                .message = "Failed to write any data",
                                .wrapperError = WrapperErrors::ShortWrite,
                            }
                        )
                    );
                }

                writePart(toWrite.substr(written), std::move(onWriteComplete));
            }
        );
    }
    ProcessingStrand* FileStream::strand() const
    {
        auto sftp = sftp_.lock();
        return sftp ? sftp->strand_.get() : nullptr;
    }
    std::expected<void, SftpError> FileStream::writeInStrand(std::string_view data)
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        VERIFY_FILE_STREAM();
        while (!data.empty())
        {
            const auto chunk = std::min(data.size(), writeLengthLimit());
            const auto written = sftp_write(file_.get(), data.data(), chunk);
            if (written < 0)
                return std::unexpected(lastError());
            if (written == 0)
                return std::unexpected(
                    SftpError{
                        .message = "Failed to write any data",
                        .wrapperError = WrapperErrors::ShortWrite,
                    }
                );
            data.remove_prefix(static_cast<std::size_t>(written));
        }
        return {};
    }

    std::future<std::expected<void, SftpError>> FileStream::write(std::string_view data)
    {
        // Short easy path:
        if (data.size() <= writeLengthLimit())
        {
            return performPromise(
                [this, data]() -> std::expected<void, SftpError>
                {
                    VERIFY_FILE_STREAM();
                    const auto written = sftp_write(file_.get(), data.data(), data.size());
                    if (written < 0)
                        return std::unexpected(lastError());
                    return {};
                }
            );
        }

        // Write in chunks for large data:
        auto promise = std::make_shared<std::promise<std::expected<void, SftpError>>>();
        writePart(
            data,
            [promise](std::expected<void, SftpError>&& result)
            {
                promise->set_value(std::move(result));
            }
        );
        return promise->get_future();
    }
    sftp_file FileStream::release()
    {
        sftp_.reset();
        limits_ = {};
        return file_.release();
    }
    std::shared_ptr<AsyncTransferContext> FileStream::makeTransferContext()
    {
        auto context = std::make_shared<AsyncTransferContext>();
        context->wake_ = [weakStream = weak_from_this()]()
        {
            if (auto stream = weakStream.lock(); stream)
            {
                if (auto sftp = stream->sftp_.lock(); sftp)
                    sftp->strand_->wake();
            }
        };
        return context;
    }

    std::expected<std::shared_ptr<AsyncTransferContext>, SftpError> FileStream::readAsyncInStrand(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<bool(SignedSizeType)> onRead
    )
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        auto context = makeTransferContext();
        const auto [success, id] = strand()->pushPermanentTask(
            [weakStream = weak_from_this(),
             buffer,
             bufferSize,
             onRead = std::move(onRead),
             context,
             totalFileSize,
             pipeline = std::make_shared<ReadPipeline>()]() -> TaskStatus
            {
                auto stream = weakStream.lock();
                auto sftp = stream ? stream->sftp_.lock() : nullptr;
                // Waiting needs the session and the open file, without them the requests are only freed.
                const auto discardPipeline = [&]()
                {
                    pipeline->discard(sftp ? stream->file_.get() : nullptr, buffer, bufferSize);
                };

                if (context->hasEnded())
                {
                    discardPipeline();
                    return TaskStatus::Done;
                }
                // Answers in flight wait in the session while paused, so nothing moves until resumed.
                if (context->paused())
                {
                    pipeline->window().restartSample();
                    return TaskStatus::Idle;
                }
                if (!stream)
                {
                    context->fail(expiredError("File stream"));
                    return TaskStatus::Done;
                }

                if (!sftp)
                {
                    discardPipeline();
                    context->fail(expiredError("SFTP session"));
                    return TaskStatus::Done;
                }
                const auto target = sftp->chunkController().targetCallDuration();
                if (stream->strand()->cycleElapsed() > target)
                    return TaskStatus::Busy;

                const auto failWith = [&](SftpError error)
                {
                    discardPipeline();
                    context->fail(std::move(error));
                    return TaskStatus::Done;
                };

                auto* const file = stream->file_.get();
                const auto lengthLimit = static_cast<SignedSizeType>(stream->readLengthLimit());
                const auto upperBound =
                    std::max(SignedSizeType{1}, lengthLimit > 0 ? std::min(bufferSize, lengthLimit) : bufferSize);
                const auto fillPipeline = [&]() -> std::expected<void, SftpError>
                {
                    const auto unrequested =
                        totalFileSize - context->bytesTransferred_.load() - pipeline->inFlightBytes();
                    return pipeline->fill(*sftp, file, unrequested, upperBound);
                };

                if (auto filled = fillPipeline(); !filled)
                    return failWith(std::move(filled).error());

                // Requests stay in flight between turns, so a quarter window per turn moves as much as
                // a whole one and hands the thread to other tasks four times as often.
                const auto turnStart = std::chrono::steady_clock::now();
                const auto turnBudget = std::max(targetBytesPerTurn, pipeline->window().windowBytes() / 4);
                SignedSizeType turnBytes = 0;
                while (
                    !pipeline->empty() && !context->paused() && turnBytes < turnBudget &&
                    !(turnBytes > 0 && turnExpired(turnStart, target))
                )
                {
                    const auto received = pipeline->receiveOldest(*sftp, buffer, bufferSize);
                    if (!received)
                        return failWith(received.error());
                    if (received->received == 0)
                    {
                        // The remote file is shorter than listed. Finalizing would replace the
                        // destination with a truncated copy.
                        context->calculateBytesPerSecond();
                        return failWith(SftpError{
                            .message = "The remote file ended before the expected size was read",
                            .wrapperError = WrapperErrors::UnexpectedEndOfFile,
                        });
                    }
                    pipeline->window().recordReceived(received->received, std::chrono::steady_clock::now(), target);
                    if (!onRead(received->received))
                    {
                        return failWith(SftpError{
                            .message = "Storing the received data failed",
                            .wrapperError = WrapperErrors::LocalIoFailure,
                        });
                    }
                    context->bytesTransferred_ += received->received;
                    turnBytes += received->received;

                    if (received->received < received->requested)
                    {
                        // A short answer leaves a gap before the requests after it: ask again from
                        // where it ended.
                        discardPipeline();
                        sftp_seek64(file, received->offset + static_cast<std::uint64_t>(received->received));
                    }
                    if (auto filled = fillPipeline(); !filled)
                        return failWith(std::move(filled).error());
                }

                context->calculateBytesPerSecond();
                if (context->bytesTransferred_.load() >= totalFileSize && pipeline->empty())
                {
                    context->complete();
                    return TaskStatus::Done;
                }
                return TaskStatus::Busy;
            }
        );
        if (!success)
            return std::unexpected(
                SftpError{
                    .message = "Failed to push permanent task",
                    .wrapperError = WrapperErrors::TaskPushFailed,
                }
            );
        context->permanentTaskId_ = id;
        return context;
    }

    std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> FileStream::readAsync(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<bool(SignedSizeType)> onRead
    )
    {
        return performPromise(
            [this, totalFileSize, buffer, bufferSize, onRead = std::move(onRead)]() mutable
            {
                return readAsyncInStrand(totalFileSize, buffer, bufferSize, std::move(onRead));
            }
        );
    }

    std::expected<std::shared_ptr<AsyncTransferContext>, SftpError> FileStream::writeAsyncInStrand(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<SignedSizeType(SignedSizeType)> doRead
    )
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        auto context = makeTransferContext();
        const auto [success, id] = strand()->pushPermanentTask(
            [weakStream = weak_from_this(), buffer, bufferSize, doRead = std::move(doRead), context, totalFileSize](
            ) -> TaskStatus
            {
                if (context->hasEnded())
                    return TaskStatus::Done;
                if (context->paused())
                    return TaskStatus::Idle;

                auto stream = weakStream.lock();
                if (!stream)
                {
                    context->fail(expiredError("File stream"));
                    return TaskStatus::Done;
                }

                auto sftp = stream->sftp_.lock();
                if (!sftp)
                {
                    context->fail(expiredError("SFTP session"));
                    return TaskStatus::Done;
                }
                const auto target = sftp->chunkController().targetCallDuration();
                if (stream->strand()->cycleElapsed() > target)
                    return TaskStatus::Busy;

                auto remainingWrite = totalFileSize - context->bytesTransferred_.load();
                const auto lengthLimit = static_cast<SignedSizeType>(stream->writeLengthLimit());
                const auto turnStart = std::chrono::steady_clock::now();
                SignedSizeType turnBytes = 0;

                while (
                    remainingWrite > 0 && turnBytes < targetBytesPerTurn &&
                    !(turnBytes > 0 && turnExpired(turnStart, target))
                )
                {
                    const auto chunk = transferChunkBytes(*sftp, bufferSize, lengthLimit);
                    const auto amountRead = doRead(std::min(remainingWrite, chunk));
                    if (amountRead <= 0)
                    {
                        context->fail(SftpError{
                            .message = "Reading the local file failed",
                            .wrapperError = WrapperErrors::LocalIoFailure,
                        });
                        return TaskStatus::Done;
                    }
                    const auto requested = std::min(remainingWrite, amountRead);
                    const auto callStart = std::chrono::steady_clock::now();
                    const auto result = sftp_write(stream->file_.get(), buffer, requested);
                    if (result < 0)
                    {
                        context->fail(transferError(*sftp, "Writing the remote file failed"));
                        return TaskStatus::Done;
                    }
                    sftp->chunkController().recordDataCall(result, chunk, std::chrono::steady_clock::now() - callStart);
                    remainingWrite -= result;
                    context->bytesTransferred_ += result;
                    turnBytes += result;
                }

                context->calculateBytesPerSecond();
                if (remainingWrite == 0)
                {
                    context->complete();
                    return TaskStatus::Done;
                }
                return TaskStatus::Busy;
            }
        );
        if (!success)
            return std::unexpected(
                SftpError{
                    .message = "Failed to push permanent task",
                    .wrapperError = WrapperErrors::TaskPushFailed,
                }
            );
        context->permanentTaskId_ = id;
        return context;
    }

    std::future<std::expected<std::shared_ptr<AsyncTransferContext>, SftpError>> FileStream::writeAsync(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<SignedSizeType(SignedSizeType)> doRead
    )
    {
        return performPromise(
            [this, totalFileSize, buffer, bufferSize, doRead = std::move(doRead)]() mutable
            {
                return writeAsyncInStrand(totalFileSize, buffer, bufferSize, std::move(doRead));
            }
        );
    }
}
