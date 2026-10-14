#include <backend/sftp/upload_operation.hpp>
#include <backend/sftp/resume_check.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>
#include <ssh/sftp_session.hpp>

#include <log/log.hpp>
#include <utility/permissions.hpp>

#include <chrono>
#include <filesystem>
#include <tuple>

UploadOperation::UploadOperation(SecureShell::SftpSession& sftp, UploadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , fileStream_{}
    , options_{std::move(options)}
    , localFile_{}
{
    options_.tempFileSuffix = Persistence::effectiveTempFileSuffix(options_.tempFileSuffix);
    remotePath_ = options_.remotePath;
    if (!options_.localPath.empty())
        localNativePath_ = options_.localPath.native();
    tempPath_ = remotePath_.withFilenameSuffix(options_.tempFileSuffix).value_or(Paths::RemotePath{});
}

UploadOperation::~UploadOperation()
{
    std::ignore = cancel(false);
    waitForStrandTasks(strand());
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
    auto fut = sftp_->performPromise(
        [this]()
        {
            return workInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("UploadOperation: work umbrella timed out.");
        return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<UploadOperation::WorkStatus, UploadOperation::Error> UploadOperation::workInStrand()
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
            // Local-only preconditions replicated from prepare(); remote work lives in
            // prepareInStrand() and runs without an inner umbrella.
            if (options_.localPath.empty())
            {
                Log::error("UploadOperation: Invalid local path.");
                return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath});
            }
            const auto localStatus = std::filesystem::symlink_status(localNativePath_);
            if (!std::filesystem::is_regular_file(localStatus) && !std::filesystem::is_symlink(localStatus))
            {
                Log::error(
                    "UploadOperation: Local path '{}' is not a regular file or symlink.", options_.localPath.toUtf8()
                );
                return enterErrorState<WorkStatus>({.type = ErrorType::NotAFile});
            }
            if (std::filesystem::is_symlink(localStatus) &&
                options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
            {
                isSymlink_ = true;
            }
            else
            {
                if (!std::filesystem::is_regular_file(std::filesystem::status(localNativePath_)))
                {
                    Log::error(
                        "UploadOperation: Local path '{}' is a directory, but a file was expected.",
                        options_.localPath.toUtf8()
                    );
                    return enterErrorState<WorkStatus>({.type = ErrorType::NotAFile});
                }
                if (!std::filesystem::exists(localNativePath_))
                {
                    Log::error("UploadOperation: Local file '{}' does not exist.", options_.localPath.toUtf8());
                    return enterErrorState<WorkStatus>({.type = ErrorType::FileNotFound});
                }
                localFile_.open(localNativePath_, std::ios::binary);
                if (!localFile_.is_open())
                {
                    Log::error("UploadOperation: Failed to open local file: {}", options_.localPath.toUtf8());
                    return enterErrorState<WorkStatus>({.type = ErrorType::OpenFailure});
                }
                localFile_.seekg(0, std::ios::end);
                leftToUpload_ = static_cast<std::size_t>(localFile_.tellg());
                totalSize_ = leftToUpload_;
                localFile_.seekg(0, std::ios::beg);
                if (!localFile_.good())
                {
                    Log::error("UploadOperation: Failed to seek in local file: {}", options_.localPath.toUtf8());
                    return enterErrorState<WorkStatus>({.type = ErrorType::FileSeekFailure});
                }
                const auto prepareResult = prepareInStrand();
                if (!prepareResult.has_value())
                {
                    Log::error("UploadOperation: Failed to prepare operation: {}", prepareResult.error().toString());
                    return enterErrorState<WorkStatus>(prepareResult.error());
                }
            }
            state_ = Prepared;
            [[fallthrough]];
        }
        case (Prepared):
        {
            state_ = Running;
            if (isSymlink_ && options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
            {
                const auto result = handleSymlinkInStrand();
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
                const auto contextResult = stream->writeAsyncInStrand(
                    static_cast<SecureShell::IFileStream::SignedSizeType>(leftToUpload_),
                    buffer_.data(),
                    buffer_.size(),
                    [this](SecureShell::IFileStream::SignedSizeType bytesRead)
                    {
                        return commitFileToBuffer(bytesRead);
                    }
                );
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
                const auto result = writeOnceInStrand();
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
                    if (asyncTransferContext_->outcome() != SecureShell::AsyncTransferContext::Outcome::Completed)
                        return enterErrorState<WorkStatus>(asyncTransferError(*asyncTransferContext_));
                    leftToUpload_ = 0;
                    options_.progressCallback(0ull, totalSize_, totalSize(), asyncTransferContext_->bytesPerSecond());
                    state_ = Finalizing;
                    [[fallthrough]];
                }
                else
                {
                    options_.progressCallback(
                        0ull,
                        totalSize_,
                        totalSize_ - leftToUpload_ +
                            static_cast<std::size_t>(asyncTransferContext_->bytesTransferred()),
                        asyncTransferContext_->bytesPerSecond()
                    );
                    return WorkStatus::MoreWork;
                }
            }
        }
        case (Finalizing):
        {
            localFile_.close();
            const auto finalizeResult = finalizeInStrand();
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
    auto fut = sftp_->performPromise(
        [this]()
        {
            return handleSymlinkInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("UploadOperation: handleSymlink umbrella timed out.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<void, UploadOperation::Error> UploadOperation::handleSymlinkInStrand()
{
    if (options_.symlinkHandling == Persistence::SymlinkHandling::SkipSymlink)
    {
        Log::info("UploadOperation: Skipping symlink as per options: {}.", options_.localPath.toUtf8());
        return {};
    }

    const auto target = std::filesystem::read_symlink(localNativePath_);

    if (options_.createMissingDirectories)
    {
        if (auto res = ensureRemoteDirectoryExistsInStrand(*remotePath_.parent()); !res.has_value())
            return std::unexpected(res.error());
    }

    // The link is created under the temp name and renamed into place, so an existing destination is
    // never removed before its replacement exists.
    std::ignore = sftp_->removeFileInStrand(tempPath_);
    const auto result = sftp_->createSymLinkInStrand(Paths::nativeToUtf8(target), tempPath_);
    if (!result.has_value())
    {
        Log::error(
            "UploadOperation: Failed to create symlink at '{}': {}.", tempPath_.value(), result.error().message
        );
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    const auto moved = sftp_->moveIntoPlaceInStrand(
        tempPath_,
        remotePath_,
        options_.mayOverwrite,
        Persistence::asideFileSuffix(options_.tempFileSuffix),
        SharedData::FileType::Symlink
    );
    if (!moved.has_value())
    {
        std::ignore = sftp_->removeFileInStrand(tempPath_);
        Log::error(
            "UploadOperation: Failed to move symlink to '{}': {}.", options_.remotePath.value(), moved.error().message
        );
        return std::unexpected(remoteMoveError(moved.error()));
    }
    return {};
}

std::expected<bool, UploadOperation::Error> UploadOperation::writeOnce()
{
    auto fut = sftp_->performPromise(
        [this]()
        {
            return writeOnceInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("UploadOperation: writeOnce umbrella timed out.");
        return enterErrorState<bool>({.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<bool, UploadOperation::Error> UploadOperation::writeOnceInStrand()
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
    if (readCount == 0 && leftToUpload_ > 0)
    {
        Log::error("UploadOperation: The local file ended before its expected size was read.");
        return enterErrorState<bool>({.type = ErrorType::SourceFileNotGood});
    }
    leftToUpload_ -= readCount;

    const auto result = stream->writeInStrand(std::string_view{buffer_.data(), readCount});
    if (!result.has_value())
    {
        Log::error("UploadOperation: Failed to write to remote file: {}", result.error().message);
        return enterErrorState<bool>({.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    options_.progressCallback(0, totalSize_, totalSize_ - leftToUpload_, 0);

    return leftToUpload_ > 0;
}

std::expected<void, UploadOperation::Error> UploadOperation::openOrAdoptFileInStrand()
{
    const auto result = sftp_->lstatInStrand(remotePath_);
    if (!result.has_value())
    {
        const auto err = result.error();
        if (err.sftpError != SSH_FX_NO_SUCH_FILE)
        {
            Log::error("Failed to stat remote sftp file for continue: {}", err.message);
            return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = err});
        }
        // else ok — target does not yet exist.
    }
    else
    {
        if (!options_.mayOverwrite)
        {
            Log::warn(
                "UploadOperation: Remote file '{}' already exists and may not be overwritten.",
                options_.remotePath.value()
            );
            return std::unexpected(Error{.type = ErrorType::FileExists});
        }
    }

    const auto tempResult = sftp_->lstatInStrand(tempPath_);

    // Permissions of what is uploaded: the target of a followed symlink, not the link.
    std::error_code statusError{};
    const auto status = std::filesystem::status(localNativePath_, statusError);
    const auto perms = determinePerms(statusError ? std::filesystem::perms::unknown : Utility::localPermissions(status));

    // Adoption mechanic for temp file parts:
    if (tempResult.has_value())
    {
        if (tempResult->size > static_cast<std::uint64_t>(leftToUpload_))
        {
            Log::debug("UploadOperation: Remote temp file is larger than local file, do not adopt file.");
            // Do not adopt file
        }
        else if (options_.tryContinue && !tempBelongsToSourceInStrand(tempResult->size))
        {
            Log::info(
                "UploadOperation: Remote temp file '{}' is not a part of this source, starting over.", tempPath_.value()
            );
        }
        else if (options_.tryContinue)
        {
            Log::debug("UploadOperation: Continuing upload to existing temp file.");

            const auto openResult = sftp_->openFileInStrand(tempPath_, SecureShell::SftpSession::OpenType::Write, perms);
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

            const auto seekResult = stream->seekInStrand(tempResult->size);
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

    if (options_.createMissingDirectories)
    {
        if (auto res = ensureRemoteDirectoryExistsInStrand(*tempPath_.parent()); !res.has_value())
            return std::unexpected(res.error());
    }

    // Open temp file part regularly, freshly:
    Log::debug("UploadOperation: Starting new upload to '{}'.", tempPath_.value());
    const auto openResult = sftp_->openFileInStrand(
        tempPath_,
        SecureShell::SftpSession::OpenType::Write | SecureShell::SftpSession::OpenType::Create |
            SecureShell::SftpSession::OpenType::Truncate,
        perms
    );
    if (!openResult.has_value())
    {
        Log::error("Failed to open remote sftp file for upload: {}", openResult.error().message);
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = openResult.error()});
    }

    fileStream_ = openResult.value();

    return {};
}

bool UploadOperation::tempBelongsToSourceInStrand(std::uint64_t tempSize)
{
    // Another program's partial file or a part of an older version of the source must not be continued.
    if (tempSize == 0)
        return false;
    const auto opened =
        sftp_->openFileInStrand(tempPath_, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::none);
    if (!opened)
        return false;
    auto stream = opened->lock();
    if (!stream)
        return false;
    const auto matches = ResumeCheck::tailsMatchInStrand(*stream, localNativePath_, tempSize);
    stream->closeInStrand();
    return matches.value_or(false);
}

std::expected<void, UploadOperation::Error>
UploadOperation::ensureRemoteDirectoryExistsInStrand(Paths::RemotePath const& dir)
{
    if (dir.isRoot())
        return {};

    const auto statResult = sftp_->lstatInStrand(dir);
    if (statResult.has_value())
    {
        if (statResult->type == SharedData::FileType::Directory)
            return {};
        Log::error(
            "UploadOperation: Cannot create parent '{}': path exists and is not a directory.", dir.value()
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = SecureShell::SftpError{
                    .message = "Parent path exists but is not a directory",
                    .sftpError = SSH_FX_FAILURE,
                },
            }
        );
    }
    if (statResult.error().sftpError != SSH_FX_NO_SUCH_FILE)
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = statResult.error()});

    if (auto parent = ensureRemoteDirectoryExistsInStrand(dir.parent().value_or(Paths::RemotePath::root()));
        !parent.has_value())
        return parent;

    const auto dirPerms = options_.directoryPermissions.value_or(
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
        std::filesystem::perms::others_read | std::filesystem::perms::others_exec
    );
    const auto mkdirResult = sftp_->createDirectoryIfItDoesntExistInStrand(dir, dirPerms);
    if (!mkdirResult.has_value())
    {
        Log::error(
            "UploadOperation: Failed to create parent dir '{}': {}.", dir.value(), mkdirResult.error().message
        );
        return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = mkdirResult.error()});
    }
    return {};
}

std::expected<void, Operation::Error> UploadOperation::prepare()
{
    if (options_.localPath.empty())
    {
        Log::error("UploadOperation: Invalid local path.");
        return enterErrorState({.type = ErrorType::InvalidPath});
    }
    if (remotePath_.empty() || remotePath_.isRoot() || tempPath_.empty())
    {
        Log::error("UploadOperation: Invalid remote path '{}'.", options_.remotePath.value());
        return enterErrorState({.type = ErrorType::InvalidPath, .extraInfo = options_.remotePath.value()});
    }
    const auto localStatus = std::filesystem::symlink_status(localNativePath_);
    if (!std::filesystem::is_regular_file(localStatus) && !std::filesystem::is_symlink(localStatus))
    {
        Log::error("UploadOperation: Local path '{}' is not a regular file or symlink.", options_.localPath.toUtf8());
        return enterErrorState({.type = ErrorType::NotAFile});
    }

    if (std::filesystem::is_symlink(localStatus) &&
        options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
    {
        isSymlink_ = true;
        return {};
    }

    if (!std::filesystem::is_regular_file(std::filesystem::status(localNativePath_)))
    {
        Log::error(
            "UploadOperation: Local path '{}' is a directory, but a file was expected.", options_.localPath.toUtf8()
        );
        return enterErrorState({.type = ErrorType::NotAFile});
    }

    // Initial check. Check again later before rename
    if (!std::filesystem::exists(localNativePath_))
    {
        Log::error("UploadOperation: Local file '{}' does not exist.", options_.localPath.toUtf8());
        return enterErrorState({.type = ErrorType::FileNotFound});
    }

    localFile_.open(localNativePath_, std::ios::binary);
    if (!localFile_.is_open())
    {
        Log::error("UploadOperation: Failed to open local file: {}", options_.localPath.toUtf8());
        return enterErrorState({.type = ErrorType::OpenFailure});
    }

    localFile_.seekg(0, std::ios::end);
    leftToUpload_ = static_cast<std::size_t>(localFile_.tellg());
    totalSize_ = leftToUpload_;
    localFile_.seekg(0, std::ios::beg);

    if (!localFile_.good())
    {
        Log::error("UploadOperation: Failed to seek in local file: {}", options_.localPath.toUtf8());
        return enterErrorState({.type = ErrorType::FileSeekFailure});
    }

    auto umbrellaFut = sftp_->performPromise(
        [this]()
        {
            return prepareInStrand();
        }
    );
    if (umbrellaFut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("UploadOperation: prepare umbrella timed out.");
        return enterErrorState({.type = ErrorType::FutureTimeout, .extraInfo = "Timeout preparing upload"});
    }
    auto openResult = umbrellaFut.get();
    if (!openResult.has_value())
    {
        Log::error("UploadOperation: Failed to open file.");
        return enterErrorState(std::move(openResult).error());
    }

    Log::debug(
        "UploadOperation: Prepared upload of '{}' to '{}'.", options_.localPath.toUtf8(), options_.remotePath.value()
    );

    return {};
}

std::expected<void, UploadOperation::Error> UploadOperation::prepareInStrand()
{
    if (auto result = openOrAdoptFileInStrand(); !result.has_value())
        return std::unexpected(result.error());

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("UploadOperation: File stream expired after open.");
        return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
    }

    buffer_ = sftp_->bufferProvider().leaseForTransfer(totalSize_, stream->writeLengthLimit());
    if (buffer_.empty())
    {
        Log::error("UploadOperation: No transfer buffer available from pool.");
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = SecureShell::SftpError{
                    .message = "No buffer available from pool",
                    .wrapperError = SecureShell::WrapperErrors::BufferUnavailable,
                },
            }
        );
    }
    return {};
}

std::expected<void, UploadOperation::Error> UploadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "UploadOperation: Upload of '{}' to '{}' canceled.",
            options_.remotePath.value(),
            options_.localPath.toUtf8()
        );
        state_ = OperationState::Canceled;
    }

    cleanup();
    return {};
}

void UploadOperation::cleanup()
{
    if (asyncTransferContext_)
        asyncTransferContext_->cancel();

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

    auto fut = sftp_->performPromise(
        [this]()
        {
            return finalizeInStrand();
        }
    );
    // finalize has more work than a single call (stat + maybe removeFile + rename + setstat),
    // so allow a bit more slack than the per-call timeout.
    if (fut.wait_for(options_.futureTimeout * 4) != std::future_status::ready)
    {
        Log::error("UploadOperation: finalize umbrella timed out.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    auto result = fut.get();
    if (!result.has_value())
        return result;

    Log::debug(
        "UploadOperation: Finalized upload of '{}' to '{}'.", options_.remotePath.value(), options_.localPath.toUtf8()
    );
    return {};
}

std::expected<void, UploadOperation::Error> UploadOperation::finalizeInStrand()
{
    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("UploadOperation: File stream expired.");
        return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
    }

    // Every byte must have been written before the temp file may replace the destination.
    if (leftToUpload_ != 0)
    {
        Log::error("UploadOperation: {} bytes were not uploaded.", leftToUpload_);
        return std::unexpected(
            Error{
                .type = ErrorType::SizeMismatch,
                .extraInfo = fmt::format(
                    "Sent {} of {} bytes for {}", totalSize_ - leftToUpload_, totalSize_, options_.localPath.toUtf8()
                ),
            }
        );
    }

    stream->closeInStrand();

    // The modification time goes on the temp file, so nothing can fail once the destination is replaced.
    applyModificationTimeInStrand();

    const auto moved = sftp_->moveIntoPlaceInStrand(
        tempPath_,
        remotePath_,
        options_.mayOverwrite,
        Persistence::asideFileSuffix(options_.tempFileSuffix),
        SharedData::FileType::Regular
    );
    if (!moved.has_value())
    {
        Log::error("UploadOperation: Failed to move '{}' into place: {}", options_.remotePath.value(), moved.error().message);
        if (auto removed = sftp_->removeFileInStrand(tempPath_); !removed.has_value())
            Log::warn("UploadOperation: Cannot remove '{}': {}", tempPath_.value(), removed.error().message);
        return std::unexpected(remoteMoveError(moved.error()));
    }
    return {};
}

void UploadOperation::applyModificationTimeInStrand()
{
    std::error_code timeError{};
    const auto localTime = std::filesystem::last_write_time(localNativePath_, timeError);
    if (timeError)
    {
        Log::warn(
            "UploadOperation: Could not read local mtime for '{}': {}.",
            options_.localPath.toUtf8(),
            Utility::errorMessage(timeError)
        );
        return;
    }
    const auto seconds = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(localTime).time_since_epoch())
            .count()
    );
    sftp_attributes_struct attributes{};
    attributes.flags = SSH_FILEXFER_ATTR_ACMODTIME;
    attributes.atime = seconds;
    attributes.mtime = seconds;
    if (const auto applied = sftp_->statInStrand(tempPath_, &attributes); !applied.has_value())
        Log::warn("UploadOperation: Failed to set mtime on '{}': {}.", tempPath_.value(), applied.error().message);
}

std::filesystem::perms UploadOperation::determinePerms(std::filesystem::perms localPerms) const
{
    const auto raw = [this, localPerms]()
    {
        if (options_.inheritPermissions)
        {
            return Utility::sanitizePermissions(localPerms).value_or(
                std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                std::filesystem::perms::others_read
            );
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