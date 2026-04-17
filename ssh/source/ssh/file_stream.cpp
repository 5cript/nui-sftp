#include <ssh/file_stream.hpp>
#include <ssh/sftp_session.hpp>

#include <cassert>
#include <utility>
#include <chrono>

namespace SecureShell
{
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
                (void)sftp->performPromise(
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
        return performPromise([this, pos]() { return seekInStrand(pos); });
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
        return performPromise([this]() { return statInStrand(); });
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
        return performPromise([this]() { return tellInStrand(); });
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
        return performPromise([this]() { return rewindInStrand(); });
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
        return performPromise([this, buffer, bufferSize]() { return readSomeInStrand(buffer, bufferSize); });
    }

    std::future<std::expected<std::size_t, SftpError>>
    FileStream::readAll(std::function<bool(std::string_view data)> onReadFunc)
    {
        struct ReadState : public std::enable_shared_from_this<ReadState>
        {
            FileStream& stream;
            std::string buffer;
            std::function<bool(std::string_view data)> callback;
            std::promise<std::expected<std::size_t, SftpError>> promise;
            std::size_t totalRead;

            ReadState(FileStream& stream, std::size_t limit, std::function<bool(std::string_view data)> callback)
                : stream{stream}
                , buffer(std::min(std::size_t{4096}, limit), '\0')
                , callback{std::move(callback)}
                , promise{}
                , totalRead{0}
            {}

            void onRead(std::make_signed_t<std::size_t> amount)
            {
                if (amount < 0)
                {
                    promise.set_value(std::unexpected(stream.lastError()));
                    callback({});
                    return;
                }

                if (amount == 0)
                {
                    promise.set_value(totalRead);
                    callback({});
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

        auto state = std::make_shared<ReadState>(*this, readLengthLimit(), std::move(onReadFunc));
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
    std::expected<std::shared_ptr<AsyncTransferContext>, SftpError> FileStream::readAsyncInStrand(
        SignedSizeType totalFileSize,
        char* buffer,
        SignedSizeType bufferSize,
        std::function<bool(SignedSizeType)> onRead
    )
    {
        if (auto sftp = sftp_.lock(); sftp)
            assert(sftp->strand_->withinProcessingThread());
        auto context = std::make_shared<AsyncTransferContext>();
        const auto [success, id] = strand()->pushPermanentTask(
            [weakStream = weak_from_this(),
                buffer,
                bufferSize,
                onRead = std::move(onRead),
                context,
                totalFileSize]()
            {
                if (context->hasEnded())
                    return false;
                if (context->paused())
                    return true;

                auto stream = weakStream.lock();
                if (!stream)
                {
                    context->cancel();
                    return false;
                }

                auto remainingRead = totalFileSize - context->bytesTransferred_.load();
                const auto readCycles =
                    std::min(SignedSizeType{10}, (remainingRead / bufferSize) + SignedSizeType{1});

                for (SignedSizeType i = 0; i != readCycles; ++i)
                {
                    const auto result = sftp_read(stream->file_.get(), buffer, std::min(remainingRead, bufferSize));
                    if (result < 0)
                    {
                        context->cancel();
                        return false;
                    }
                    remainingRead -= result;
                    if (!onRead(result))
                    {
                        context->cancel();
                        return false;
                    }
                    context->bytesTransferred_ += result;
                }

                context->calculateBytesPerSecond();
                if (remainingRead == 0)
                {
                    context->ended_ = true;
                    return false;
                }
                return true;
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
            [this, totalFileSize, buffer, bufferSize, onRead = std::move(onRead)]() mutable {
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
        auto context = std::make_shared<AsyncTransferContext>();
        const auto [success, id] = strand()->pushPermanentTask(
            [weakStream = weak_from_this(),
                buffer,
                bufferSize,
                doRead = std::move(doRead),
                context,
                totalFileSize]()
            {
                if (context->hasEnded())
                    return false;
                if (context->paused())
                    return true;

                auto stream = weakStream.lock();
                if (!stream)
                {
                    context->cancel();
                    return false;
                }

                auto remainingWrite = totalFileSize - context->bytesTransferred_.load();
                const auto writeCycles =
                    std::min(SignedSizeType{10}, (remainingWrite / bufferSize) + SignedSizeType{1});

                for (SignedSizeType i = 0; i != writeCycles; ++i)
                {
                    const auto amountRead = doRead(remainingWrite);
                    if (amountRead <= 0)
                    {
                        context->cancel();
                        return false;
                    }
                    const auto result =
                        sftp_write(stream->file_.get(), buffer, std::min(remainingWrite, amountRead));
                    if (result < 0)
                    {
                        context->cancel();
                        return false;
                    }
                    remainingWrite -= result;
                    context->bytesTransferred_ += result;
                }

                context->calculateBytesPerSecond();
                if (remainingWrite == 0)
                {
                    context->ended_ = true;
                    return false;
                }
                return true;
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
            [this, totalFileSize, buffer, bufferSize, doRead = std::move(doRead)]() mutable {
                return writeAsyncInStrand(totalFileSize, buffer, bufferSize, std::move(doRead));
            }
        );
    }
}
