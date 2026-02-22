#include <backend/sftp/upload_operation.hpp>
#include <ssh/sftp_session.hpp>

#include <log/log.hpp>
#include <tuple>

UploadOperation::UploadOperation(SecureShell::SftpSession& sftp, UploadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , fileStream_{}
    , options_{std::move(options)}
    , localFile_{}
{
    if (options_.tempFileSuffix.empty())
        options_.tempFileSuffix = ".filepart";
    if (options_.tempFileSuffix.find('/') != 0)
        options_.tempFileSuffix = ".filepart";
}

UploadOperation::~UploadOperation()
{
    std::ignore = cancel(false);
    if (auto* stra = strand(); stra)
        stra->pushPromiseTask([]() {}).get();
}

SecureShell::ProcessingStrand* UploadOperation::strand() const
{
    if (!sftp_)
        return nullptr;
    return sftp_->strand();
}

void UploadOperation::pause(bool doPause)
{
    if (asyncTransferContext_)
        asyncTransferContext_->pause(doPause);
}

std::expected<UploadOperation::WorkStatus, UploadOperation::Error> UploadOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        {
            state_ = Preparing;
            [[fallthrough]];
        }
        case (Preparing):
        {
            const auto prepareResult = prepare();
            if (!prepareResult.has_value())
            {
                Log::error("UploadOperation: Failed to prepare operation: {}", prepareResult.error().toString());
                return enterErrorState<WorkStatus>(prepareResult.error());
            }
            state_ = Prepared;
            [[fallthrough]];
        }
        case (Prepared):
        {
            state_ = Running;
            if (isSymlink_ && options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
            {
                const auto result = handleSymlink();
                if (!result.has_value())
                {
                    Log::error("UploadOperation: Failed to handle symlink: {}", result.error().toString());
                    return enterErrorState<WorkStatus>(result.error());
                }
                Log::info("UploadOperation: Symlink handled, no file to upload.");
                state_ = Completed;
                // to show 100%
                options_.progressCallback(1, 1, 1, 0);
                return WorkStatus::Complete;
            }
            if (options_.bigFileOptimized)
            {
                auto stream = fileStream_.lock();
                if (!stream)
                {
                    Log::error("UploadOperation: File stream expired.");
                    return enterErrorState<WorkStatus>({.type = ErrorType::FileStreamExpired});
                }
                auto contextFut = stream->writeAsync(
                    totalSize_,
                    buffer_.data(),
                    buffer_.size(),
                    [this](SecureShell::IFileStream::SignedSizeType bytesRead)
                    {
                        return commitFileToBuffer(bytesRead);
                    }
                );
                const auto futureStatus = contextFut.wait_for(options_.futureTimeout);
                if (futureStatus != std::future_status::ready)
                {
                    Log::error("UploadOperation: Future timed out while starting async read.");
                    return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
                }
                const auto contextResult = contextFut.get();
                if (!contextResult.has_value())
                {
                    Log::error("UploadOperation: Failed to start async read: {}", contextResult.error().toString());
                    return enterErrorState<WorkStatus>(
                        {.type = ErrorType::SftpError, .sftpError = contextResult.error()}
                    );
                }
                asyncTransferContext_ = contextResult.value();
            }
            [[fallthrough]];
        }
        case (Running):
        {
            if (!options_.bigFileOptimized)
            {
                const auto result = writeOnce();
                if (!result.has_value())
                {
                    Log::error("UploadOperation: Failed to write file: {}", result.error().toString());
                    return enterErrorState<WorkStatus>(result.error());
                }
                if (result.value())
                {
                    return WorkStatus::MoreWork;
                }
                // No More to write?
                else
                {
                    state_ = Finalizing;
                    [[fallthrough]];
                }
            }
            else
            {
                if (asyncTransferContext_->hasEnded())
                {
                    options_.progressCallback(0ull, totalSize_, totalSize(), asyncTransferContext_->bytesPerSecond());
                    state_ = Finalizing;
                    [[fallthrough]];
                }
                else
                {
                    options_.progressCallback(
                        0ull,
                        totalSize_,
                        asyncTransferContext_->bytesTransferred(),
                        asyncTransferContext_->bytesPerSecond()
                    );
                    return WorkStatus::MoreWork;
                }
            }
        }
        case (Finalizing):
        {
            const auto finalizeResult = finalize();
            if (!finalizeResult.has_value())
            {
                Log::error("UploadOperation: Failed to finalize operation: {}", finalizeResult.error().toString());
                return enterErrorState<WorkStatus>(finalizeResult.error());
            }
            state_ = Completed;
            Log::debug("UploadOperation: Operation completed successfully.");
            return WorkStatus::Complete;
        }
        case (Completed):
        {
            Log::warn("UploadOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("UploadOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("UploadOperation: Operation was canceled.");
            // Do not enter error state here, it would overwrite the cancel state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("UploadOperation: Operation completed with partial success.");
            // Do not enter error state here, it would overwrite the partial success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    Log::error("UploadOperation: Unknown operation state: {}", static_cast<int>(state_));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

SecureShell::IFileStream::SignedSizeType
UploadOperation::commitFileToBuffer(SecureShell::IFileStream::SignedSizeType bytes)
{
    localFile_.read(
        buffer_.data(), std::min(static_cast<std::streamsize>(buffer_.size()), static_cast<std::streamsize>(bytes))
    );
    if (!localFile_.good())
    {
        Log::error("UploadOperation write cycle stopped: localFile_.good() == false");
        std::ignore = enterErrorState({
            .type = SharedData::OperationErrorType::SourceFileNotGood,
        });
        return -1;
    }
    return localFile_.gcount();
}

std::expected<void, UploadOperation::Error> UploadOperation::handleSymlink()
{
    if (options_.symlinkHandling == Persistence::SymlinkHandling::SkipSymlink)
    {
        Log::info("UploadOperation: Skipping symlink as per options: {}.", options_.localPath.string());
        return {};
    }

    const auto target = std::filesystem::read_symlink(options_.localPath);

    auto fut = sftp_->createSymLink(target, options_.remotePath);
    const auto futureStatus = fut.wait_for(options_.futureTimeout);
    if (futureStatus != std::future_status::ready)
    {
        Log::error("UploadOperation: Future timed out while creating symlink.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    const auto result = fut.get();
    if (!result.has_value())
    {
        Log::error(
            "UploadOperation: Failed to create symlink at '{}': {}.",
            options_.remotePath.generic_string(),
            result.error().message
        );
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = result.error()});
    }
    return {};
}

std::expected<bool, UploadOperation::Error> UploadOperation::writeOnce()
{
    if (state_ < OperationState::Prepared)
    {
        Log::error("UploadOperation: Operation not prepared.");
        return enterErrorState<bool>({.type = ErrorType::OperationNotPrepared});
    }

    if (!localFile_.is_open())
    {
        Log::error("UploadOperation: File is not open.");
        return enterErrorState<bool>({.type = ErrorType::OpenFailure});
    }

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("UploadOperation: File stream expired.");
        return enterErrorState<bool>({.type = ErrorType::FileStreamExpired});
    }

    localFile_.read(buffer_.data(), static_cast<std::streamsize>(std::min(buffer_.size(), leftToUpload_)));
    const auto readCount = static_cast<std::size_t>(localFile_.gcount());
    if (readCount > leftToUpload_)
    {
        Log::error("UploadOperation: Read more data than expected.");
        return enterErrorState<bool>({.type = ErrorType::ImplementationError});
    }
    leftToUpload_ -= readCount;

    auto future = stream->write(std::string_view{buffer_.data(), readCount});

    const auto futureStatus = future.wait_for(options_.futureTimeout);

    if (futureStatus != std::future_status::ready)
    {
        Log::error("UploadOperation: Future timed out while reading.");
        return enterErrorState<bool>({.type = ErrorType::FutureTimeout});
    }

    const auto result = future.get();

    if (!result.has_value())
    {
        Log::error("UploadOperation: Failed to read from remote file: {}", result.error().message);
        return enterErrorState<bool>({.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    options_.progressCallback(0, totalSize_, totalSize_ - leftToUpload_, 0);

    return leftToUpload_ > 0;
}

std::expected<void, UploadOperation::Error> UploadOperation::openOrAdoptFile()
{
    auto fut = sftp_->lstat(options_.remotePath);

    const auto futureStatus = fut.wait_for(options_.futureTimeout);
    if (futureStatus != std::future_status::ready)
    {
        Log::error("Failed to stat remote sftp file for continue: timeout");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }

    const auto result = fut.get();
    if (!result.has_value())
    {
        const auto err = result.error();
        if (err.sftpError != SSH_FX_NO_SUCH_FILE)
        {
            Log::error("Failed to stat remote sftp file for continue: {}", err.message);
            return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = err});
        }
        // else ok!
    }
    else
    {
        if (!options_.mayOverwrite)
        {
            Log::warn(
                "UploadOperation: Remote file '{}' already exists and may not be overwritten.",
                options_.remotePath.generic_string()
            );
            return std::unexpected(Error{.type = ErrorType::FileExists});
        }
    }

    const auto tempPath = options_.remotePath.generic_string() + options_.tempFileSuffix;
    auto tempFuture = sftp_->lstat(tempPath);

    const auto futureStatus2 = tempFuture.wait_for(options_.futureTimeout);
    if (futureStatus2 != std::future_status::ready)
    {
        Log::error("Failed to stat remote sftp file for continue: timeout");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }

    // local file perms
    const auto status = std::filesystem::symlink_status(options_.localPath);
    const auto perms = determinePerms(status.permissions());

    // Adoption mechanic for temp file parts:
    const auto tempResult = tempFuture.get();
    if (tempResult.has_value())
    {
        if (tempResult->size > static_cast<std::uint64_t>(leftToUpload_))
        {
            Log::debug("UploadOperation: Remote temp file is larger than local file, do not adopt file.");
            // Do not adopt file
        }
        else if (options_.tryContinue)
        {
            Log::debug("UploadOperation: Continuing upload to existing temp file.");

            auto openFut = sftp_->openFile(tempPath, SecureShell::SftpSession::OpenType::Write, perms);

            const auto openStatus = openFut.wait_for(options_.futureTimeout);
            if (openStatus != std::future_status::ready)
            {
                Log::error("Failed to open remote sftp file for continue: timeout");
                return std::unexpected(Error{.type = ErrorType::FutureTimeout});
            }

            const auto openResult = openFut.get();
            if (!openResult.has_value())
            {
                Log::error("Failed to open remote sftp file for continue: {}", openResult.error().message);
                return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = openResult.error()});
            }
            fileStream_ = openResult.value();

            auto stream = fileStream_.lock();
            if (!stream)
            {
                Log::error("UploadOperation: File stream expired.");
                return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
            }

            // Seek to end of file
            auto seekFut = stream->seek(tempResult->size);
            const auto seekStatus = seekFut.wait_for(options_.futureTimeout);
            if (seekStatus != std::future_status::ready)
            {
                Log::error("Failed to seek remote sftp file for continue: timeout");
                return std::unexpected(Error{.type = ErrorType::FutureTimeout});
            }

            const auto seekResult = seekFut.get();
            if (!seekResult.has_value())
            {
                Log::error("Failed to seek remote sftp file for continue: {}", seekResult.error().message);
                return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = seekResult.error()});
            }

            // Success! Continue upload from position.
            leftToUpload_ -= static_cast<std::size_t>(tempResult->size);
            localFile_.seekg(static_cast<std::streamoff>(tempResult->size));
            return {};
        }
    }

    // Open temp file part regularly, freshly:
    Log::debug("UploadOperation: Starting new upload to '{}'.", tempPath);
    auto openFut = sftp_->openFile(
        tempPath,
        SecureShell::SftpSession::OpenType::Write | SecureShell::SftpSession::OpenType::Create |
            SecureShell::SftpSession::OpenType::Truncate,
        perms
    );
    const auto openStatus = openFut.wait_for(options_.futureTimeout);
    if (openStatus != std::future_status::ready)
    {
        Log::error("Failed to open remote sftp file for upload: timeout");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    const auto openResult = openFut.get();
    if (!openResult.has_value())
    {
        Log::error("Failed to open remote sftp file for upload: {}", openResult.error().message);
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = openResult.error()});
    }

    fileStream_ = openResult.value();

    return {};
}

std::expected<void, Operation::Error> UploadOperation::prepare()
{
    if (options_.localPath.empty())
    {
        Log::error("UploadOperation: Invalid local path.");
        return enterErrorState({.type = ErrorType::InvalidPath});
    }
    const auto localStatus = std::filesystem::symlink_status(options_.localPath);
    if (!std::filesystem::is_regular_file(localStatus) && !std::filesystem::is_symlink(localStatus))
    {
        Log::error(
            "UploadOperation: Local path '{}' is not a regular file or symlink.", options_.localPath.generic_string()
        );
        return enterErrorState({.type = ErrorType::NotAFile});
    }

    if (std::filesystem::is_symlink(localStatus) &&
        options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
    {
        isSymlink_ = true;
        return {};
    }

    if (!std::filesystem::is_regular_file(std::filesystem::status(options_.localPath)))
    {
        Log::error(
            "UploadOperation: Local path '{}' is a directory, but a file was expected.",
            options_.localPath.generic_string()
        );
        return enterErrorState({.type = ErrorType::NotAFile});
    }

    // Initial check. Check again later before rename
    if (!std::filesystem::exists(options_.localPath))
    {
        Log::error("UploadOperation: Local file '{}' does not exist.", options_.localPath.generic_string());
        return enterErrorState({.type = ErrorType::FileNotFound});
    }

    localFile_.open(options_.localPath, std::ios::binary);
    if (!localFile_.is_open())
    {
        Log::error("UploadOperation: Failed to open local file: {}", options_.localPath.generic_string());
        return enterErrorState({.type = ErrorType::OpenFailure});
    }

    localFile_.seekg(0, std::ios::end);
    leftToUpload_ = static_cast<std::size_t>(localFile_.tellg());
    totalSize_ = leftToUpload_;
    localFile_.seekg(0, std::ios::beg);

    if (!localFile_.good())
    {
        Log::error("UploadOperation: Failed to seek in local file: {}", options_.localPath.generic_string());
        return enterErrorState({.type = ErrorType::FileSeekFailure});
    }

    auto openResult = openOrAdoptFile();
    if (!openResult.has_value())
    {
        Log::error("UploadOperation: Failed to open file.");
        return enterErrorState(std::move(openResult).error());
    }

    Log::debug(
        "UploadOperation: Prepared upload of '{}' to '{}'.",
        options_.localPath.generic_string(),
        options_.remotePath.generic_string()
    );

    return {};
}

std::expected<void, UploadOperation::Error> UploadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "UploadOperation: Upload of '{}' to '{}' canceled.",
            options_.remotePath.generic_string(),
            options_.localPath.generic_string()
        );
        state_ = OperationState::Canceled;
    }

    cleanup();
    return {};
}

void UploadOperation::cleanup()
{
    localFile_.close();

    if (auto stream = fileStream_.lock(); stream)
        stream->close(false);
}

std::expected<void, UploadOperation::Error> UploadOperation::finalize()
{
    if (state_ == OperationState::Running)
    {
        Log::error("UploadOperation: Cannot finalize while reading.");
        return std::unexpected(Error{.type = ErrorType::CannotFinalizeDuringRead});
    }

    localFile_.close();

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("UploadOperation: File stream expired.");
        return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
    }

    stream->close();

    // Recheck if it exists or not:
    {
        auto fut = sftp_->stat(options_.remotePath);

        if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
        {
            Log::error("Failed to stat remote sftp file for continue: timeout");
            return std::unexpected(Error{.type = ErrorType::FutureTimeout});
        }

        const auto result = fut.get();
        if (!result.has_value())
        {
            // interpret error as ok. it either does not exist or something else is wrong, but
            // try to rename anyway.
        }
        else
        {
            if (!options_.mayOverwrite)
            {
                Log::warn(
                    "UploadOperation: Remote file '{}' already exists and may not be overwritten.",
                    options_.remotePath.generic_string()
                );
                return std::unexpected(Error{.type = ErrorType::FileExists});
            }
            else
            {
                sftp_->removeFile(options_.remotePath);
            }
        }
    }

    auto fut = sftp_->rename(options_.remotePath.generic_string() + options_.tempFileSuffix, options_.remotePath);

    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("Failed to rename remote sftp file: timeout");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    const auto result = fut.get();
    if (!result.has_value())
    {
        Log::error("Failed to rename remote sftp file: {}", result.error().message);
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    Log::debug(
        "UploadOperation: Finalized upload of '{}' to '{}'.",
        options_.remotePath.generic_string(),
        options_.localPath.generic_string()
    );
    return {};
}

std::filesystem::perms UploadOperation::determinePerms(std::filesystem::perms localPerms) const
{
    const auto raw = [this, localPerms]()
    {
        if (options_.inheritPermissions)
        {
            return localPerms;
        }
        else
        {
            if (options_.filePermissions)
                return options_.filePermissions.value();
            // inherit execute permissions anyway:
            return (
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read | std::filesystem::perms::group_write
            );
        }
    }();

    // Owner has to write obviously for the upload:
    return raw | std::filesystem::perms::owner_write;
}