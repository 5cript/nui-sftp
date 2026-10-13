#include <backend/sftp/download_operation.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>

#include <log/log.hpp>
#include <utility/permissions.hpp>
#include <utility/download_mark.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/rename.hpp>

#include <tuple>

using namespace std::chrono_literals;

DownloadOperation::DownloadOperation(SecureShell::SftpSession& sftp, DownloadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
    , localFile_{}
{
    options_.tempFileSuffix = Persistence::effectiveTempFileSuffix(options_.tempFileSuffix);
    remotePath_ = options_.remotePath;
    if (!options_.localPath.empty())
        localNativePath_ = options_.localPath.native();
}

DownloadOperation::~DownloadOperation()
{
    std::ignore = cancel(false);

    if (auto stream = fileStream_.lock(); stream && !stream->strand()->withinProcessingThread())
    {
        // Wait for all tasks of the operation to finish. Skipped on the processing thread itself,
        // where the wait could never be satisfied.
        stream->strand()->pushPromiseTask([]() {}).get();
    }
}

std::expected<DownloadOperation::WorkStatus, DownloadOperation::Error> DownloadOperation::work()
{
    // Drive one work() step from outside the strand. OperationQueue normally calls
    // workInStrand directly inside its batched umbrella, so this wrapper is only used by
    // callers that aren't already on the strand (standalone ops, tests).
    auto fut = sftp_->performPromise(
        [this]()
        {
            return workInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: work umbrella timed out.");
        return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<DownloadOperation::WorkStatus, DownloadOperation::Error> DownloadOperation::workInStrand()
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
            // Local-only preconditions replicated from prepare(); the remote work runs in
            // prepareInStrand() without an inner umbrella.
            if (options_.localPath.empty())
            {
                Log::error("DownloadOperation: Invalid local path.");
                return enterErrorState<WorkStatus>({.type = ErrorType::InvalidPath});
            }
            if (std::filesystem::exists(localNativePath_) && !options_.mayOverwrite)
            {
                Log::error(
                    "DownloadOperation: File '{}' already exists and may not be overwritten.",
                    options_.localPath.toUtf8()
                );
                return enterErrorState<WorkStatus>({.type = ErrorType::FileExists});
            }
            const auto prepareResult = prepareInStrand();
            if (!prepareResult.has_value())
            {
                Log::error("DownloadOperation: Failed to prepare operation: {}", prepareResult.error().toString());
                return enterErrorState<WorkStatus>(prepareResult.error());
            }
            state_ = Prepared;
            [[fallthrough]];
        }
        case (Prepared):
        {
            state_ = Running;
            if (options_.entry->isSymlink() && options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
            {
                const auto result = handleSymlinkInStrand();
                if (!result.has_value())
                {
                    Log::error("DownloadOperation: Failed to handle symlink: {}", result.error().toString());
                    return enterErrorState<WorkStatus>(result.error());
                }
                state_ = Completed;
                // to show 100%
                options_.progressCallback(1, 1, 1, 0);
                return WorkStatus::Complete;
            }
            if (transferComplete_)
            {
                state_ = Finalizing;
                return WorkStatus::MoreWork;
            }
            if (options_.bigFileOptimized)
            {
                auto stream = fileStream_.lock();
                if (!stream)
                {
                    Log::error("DownloadOperation: File stream expired.");
                    return enterErrorState<WorkStatus>({.type = ErrorType::FileStreamExpired});
                }
                const auto contextResult = stream->readAsyncInStrand(
                    static_cast<SecureShell::IFileStream::SignedSizeType>(options_.entry->size - resumeOffset_),
                    buffer_.data(),
                    buffer_.size(),
                    [this](SecureShell::IFileStream::SignedSizeType bytesRead)
                    {
                        return commitBufferToFile(bytesRead);
                    }
                );
                if (!contextResult.has_value())
                {
                    Log::error("DownloadOperation: Failed to start async read: {}", contextResult.error().toString());
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
                const auto result = readOnceInStrand();
                if (!result.has_value())
                {
                    Log::error("DownloadOperation: Failed to read file: {}", result.error().toString());
                    return enterErrorState<WorkStatus>(result.error());
                }
                if (result.value())
                {
                    return WorkStatus::MoreWork;
                }
                // No More to read?
                else
                {
                    options_.progressCallback(0ull, 0ull, 0ull, 0ull);
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
                    options_.progressCallback(
                        0ull, options_.entry->size, options_.entry->size, asyncTransferContext_->bytesPerSecond()
                    );
                    state_ = Finalizing;
                    [[fallthrough]];
                }
                else
                {
                    options_.progressCallback(
                        0ull,
                        options_.entry->size,
                        resumeOffset_ + static_cast<std::uint64_t>(asyncTransferContext_->bytesTransferred()),
                        asyncTransferContext_->bytesPerSecond()
                    );
                    return WorkStatus::MoreWork;
                }
            }
        }
        case (Finalizing):
        {
            const auto finalizeResult = finalizeInStrand();
            if (!finalizeResult.has_value())
            {
                Log::error("DownloadOperation: Failed to finalize operation: {}", finalizeResult.error().toString());
                return enterErrorState<WorkStatus>(finalizeResult.error());
            }
            state_ = Completed;
            return WorkStatus::Complete;
        }
        case (Completed):
        {
            Log::warn("DownloadOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("DownloadOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("DownloadOperation: Operation was canceled.");
            // Do not enter error state here, it would overwrite the cancel state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("DownloadOperation: Operation completed with partial success.");
            // Do not enter error state here, it would overwrite the partial success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
    Log::error("DownloadOperation: Unknown operation state: {}", static_cast<int>(state_));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, DownloadOperation::Error> DownloadOperation::handleSymlink()
{
    auto fut = sftp_->performPromise(
        [this]()
        {
            return handleSymlinkInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: handleSymlink umbrella timed out.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<void, DownloadOperation::Error> DownloadOperation::handleSymlinkInStrand()
{
    if (options_.symlinkHandling == Persistence::SymlinkHandling::SkipSymlink)
    {
        Log::info("DownloadOperation: Skipping symlink as per options: {}.", options_.remotePath.value());
        return {};
    }

    const auto readResult = readSymlinkInStrand(remotePath_);
    if (!readResult.has_value())
        return std::unexpected(readResult.error());
    auto const& linkEntry = readResult.value().targetInfo;

    std::error_code ec{};
    if (options_.createMissingDirectories)
    {
        std::filesystem::create_directories(localNativePath_.parent_path(), ec);
        if (ec)
        {
            Log::error(
                "DownloadOperation: Failed to create parent dirs for symlink '{}': {}.",
                options_.localPath.toUtf8(),
                ec.message()
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }
    // The copy keeps the link's own content, so a relative link still points into the downloaded tree.
    const auto linkContent = Paths::nativeFromUtf8(readResult.value().linkContent);
    if (linkEntry && linkEntry->isDirectory())
        std::filesystem::create_directory_symlink(linkContent, localNativePath_, ec);
    else
        std::filesystem::create_symlink(linkContent, localNativePath_, ec);
    if (ec)
    {
        Log::error(
            "DownloadOperation: Failed to create symlink at '{}': {}.", options_.localPath.toUtf8(), ec.message()
        );
        return std::unexpected(Error{.type = ErrorType::CannotCreateSymlink});
    }
    return {};
}

std::expected<bool, DownloadOperation::Error> DownloadOperation::readOnce()
{
    auto fut = sftp_->performPromise(
        [this]()
        {
            return readOnceInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: readOnce umbrella timed out.");
        return enterErrorState<bool>({.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<bool, DownloadOperation::Error> DownloadOperation::readOnceInStrand()
{
    if (state_ < OperationState::Prepared)
    {
        Log::error("DownloadOperation: Operation not prepared.");
        return enterErrorState<bool>({.type = ErrorType::OperationNotPrepared});
    }

    if (!localFile_.is_open())
    {
        Log::error("DownloadOperation: File is not open.");
        return enterErrorState<bool>({.type = ErrorType::OpenFailure});
    }

    if (options_.entry->size == 0)
    {
        // File is empty nothing todo.
        return false;
    }

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("DownloadOperation: File stream expired.");
        return enterErrorState<bool>({.type = ErrorType::FileStreamExpired});
    }

    const auto result = stream->readSomeInStrand(buffer_.data(), buffer_.size());

    if (!result.has_value())
    {
        Log::error("DownloadOperation: Failed to read from remote file: {}", result.error().message);
        return enterErrorState<bool>({.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    const auto readAmount = result.value();

    if (readAmount == 0)
    {
        return false;
    }

    std::uint64_t fileSize = 0;
    bool good = true;
    {
        localFile_.write(buffer_.data(), static_cast<std::streamsize>(readAmount));
        bytesInFile_ += static_cast<std::uint64_t>(readAmount);
        fileSize = options_.entry->size;
        good = localFile_.good();
        options_.progressCallback(0ull, fileSize, bytesInFile_, 0);
    }
    if (!good)
    {
        Log::error("DownloadOperation read cycle stopped: localFile_.good() == false");
        std::ignore = enterErrorState({
            .type = SharedData::OperationErrorType::TargetFileNotGood,
        });
        return false;
    }
    return good && bytesInFile_ < fileSize;
}

bool DownloadOperation::commitBufferToFile(SecureShell::IFileStream::SignedSizeType bytesRead)
{
    localFile_.write(buffer_.data(), static_cast<std::streamsize>(bytesRead));
    if (!localFile_.good())
    {
        Log::error("DownloadOperation read cycle stopped: localFile_.good() == false");
        std::ignore = enterErrorState({
            .type = SharedData::OperationErrorType::TargetFileNotGood,
        });
        return false;
    }
    return true;
}

std::expected<void, DownloadOperation::Error>
DownloadOperation::openOrAdoptFileInStrand(SecureShell::IFileStream& stream)
{
    auto tempPath = localNativePath_;
    tempPath += options_.tempFileSuffix;

    if (options_.createMissingDirectories)
    {
        std::error_code mkErr{};
        std::filesystem::create_directories(tempPath.parent_path(), mkErr);
        if (mkErr)
        {
            Log::error(
                "DownloadOperation: Failed to create parent directories for '{}': {}.",
                tempPath.generic_string(),
                mkErr.message()
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }

    if (options_.tryContinue && std::filesystem::exists(tempPath))
    {
        // The size on disk, tellp right after opening in append mode reports 0 on Windows.
        std::error_code sizeError{};
        const auto existingSize = std::filesystem::file_size(tempPath, sizeError);
        if (sizeError)
        {
            Log::error("DownloadOperation: Cannot read the size of '{}': {}", tempPath.generic_string(), sizeError.message());
            return std::unexpected(Error{.type = ErrorType::FileStatFailed});
        }

        // A leftover of another program or of an older version of the source must not be continued.
        bool belongsToSource = existingSize > 0 && existingSize <= options_.entry->size;
        if (belongsToSource)
        {
            const auto matches = ResumeCheck::tailsMatchInStrand(stream, tempPath, existingSize);
            if (!matches)
                return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = matches.error()});
            belongsToSource = *matches;
        }

        if (belongsToSource && existingSize == options_.entry->size)
        {
            Log::info(
                "DownloadOperation: File '{}' already complete, will be renamed in finalize() step.",
                tempPath.generic_string()
            );
            transferComplete_ = true;
            return {};
        }
        else if (!belongsToSource)
        {
            Log::info(
                "DownloadOperation: File '{}' is larger than expected, discarding and starting over.",
                tempPath.generic_string()
            );
            localFile_.open(tempPath, std::ios::binary | std::ios::trunc);
        }
        else
        {
            Log::info("DownloadOperation: File '{}' is incomplete, continuing download.", tempPath.generic_string());
            localFile_.open(tempPath, std::ios::binary | std::ios::app);
            if (!localFile_.is_open())
            {
                Log::error("DownloadOperation: Failed to open file for appending: {}", tempPath.generic_string());
                return std::unexpected(Error{.type = ErrorType::OpenFailure});
            }
            resumeOffset_ = existingSize;
            bytesInFile_ = existingSize;
            auto seekResult = stream.seekInStrand(resumeOffset_);
            if (!seekResult.has_value())
                return std::unexpected(Error{.type = ErrorType::FileStatFailed});
        }
    }
    else
    {
        localFile_.open(tempPath, std::ios::binary | std::ios::trunc);
    }

    if (!localFile_.is_open())
    {
        Log::error("DownloadOperation: Failed to open file: {}", tempPath.generic_string());
        return std::unexpected(Error{.type = ErrorType::OpenFailure});
    }

    return {};
}

std::expected<SecureShell::SftpSession::DeepLinkResult, DownloadOperation::Error>
DownloadOperation::readSymlinkInStrand(Paths::RemotePath const& remoteFullPath)
{
    auto deepLinkResult = sftp_->readLinkDeepInStrand(remoteFullPath);
    if (!deepLinkResult.has_value())
    {
        Log::error(
            "DownloadOperation: Failed to read symlink target: {} for entry: {}.",
            deepLinkResult.error().toString(),
            remoteFullPath.value()
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = deepLinkResult.error(),
                .extraInfo = fmt::format("Reading symlink target for entry: {}", remoteFullPath.value())
            }
        );
    }
    return deepLinkResult.value();
}

std::expected<void, Operation::Error> DownloadOperation::prepare()
{
    if (options_.localPath.empty())
    {
        Log::error("DownloadOperation: Invalid local path.");
        return enterErrorState({.type = ErrorType::InvalidPath});
    }

    // Initial check. Check again later before rename.
    if (std::filesystem::exists(localNativePath_))
    {
        if (!options_.mayOverwrite)
        {
            Log::error(
                "DownloadOperation: File '{}' already exists and may not be overwritten.", options_.localPath.toUtf8()
            );
            return enterErrorState({.type = ErrorType::FileExists});
        }
    }

    auto fut = sftp_->performPromise(
        [this]()
        {
            return prepareInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: prepare umbrella timed out.");
        return enterErrorState({.type = ErrorType::FutureTimeout, .extraInfo = "Timeout preparing download"});
    }
    auto result = fut.get();
    if (!result.has_value())
        return enterErrorState(std::move(result).error());
    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::prepareInStrand()
{
    if (remotePath_.empty() || remotePath_.isRoot())
    {
        Log::error("DownloadOperation: Invalid remote path '{}'.", options_.remotePath.value());
        return std::unexpected(Error{.type = ErrorType::InvalidPath, .extraInfo = options_.remotePath.value()});
    }

    if (!options_.entry)
    {
        const auto fileInfo = sftp_->lstatInStrand(remotePath_);
        if (!fileInfo.has_value())
        {
            Log::error("DownloadOperation: Failed to stat file.");
            return std::unexpected(Error{.type = ErrorType::FileStatFailed, .sftpError = fileInfo.error()});
        }
        options_.entry = fileInfo.value();
    }

    // Dont try to open symlink as file, unless option says to download said file as a file:
    if (options_.entry->isSymlink() && options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
        return {};

    const auto streamOpenResult = sftp_->openFileInStrand(
        remotePath_, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown
    );
    if (!streamOpenResult.has_value())
    {
        Log::error("DownloadOperation: Failed to open remote sftp file: {}.", streamOpenResult.error().message);
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = streamOpenResult.error(),
                .extraInfo = fmt::format("Opening remote file: {}", options_.remotePath.value())
            }
        );
    }

    fileStream_ = streamOpenResult.value();
    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("DownloadOperation: File stream expired after opening.");
        return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
    }

    if (options_.entry->isSymlink())
    {
        // Following a link transfers its target, so the target's size and times apply.
        const auto targetInfo = stream->statInStrand();
        if (!targetInfo.has_value())
        {
            Log::error("DownloadOperation: Failed to stat the symlink target.");
            return std::unexpected(Error{.type = ErrorType::FileStatFailed, .sftpError = targetInfo.error()});
        }
        if (!targetInfo->isRegularFile())
            return std::unexpected(Error{.type = ErrorType::NotAFile});
        options_.entry->type = targetInfo->type;
        options_.entry->size = targetInfo->size;
        options_.entry->mtime = targetInfo->mtime;
        options_.entry->mtimeNsec = targetInfo->mtimeNsec;
        options_.entry->permissions = targetInfo->permissions;
    }

    auto openResult = openOrAdoptFileInStrand(*stream);
    if (!openResult.has_value())
    {
        Log::error("DownloadOperation: Failed to open file.");
        return std::unexpected(openResult.error());
    }

    buffer_ = sftp_->bufferProvider().leaseForTransfer(options_.entry->size, stream->readLengthLimit());
    if (buffer_.empty())
    {
        Log::error("DownloadOperation: No transfer buffer available from pool.");
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

    if (options_.reserveSpace && options_.entry->size != 0)
    {
        const auto pos = localFile_.tellp();
        localFile_.seekp(options_.entry->size - 1);
        localFile_.put('\0');
        if (localFile_.fail())
        {
            Log::error("DownloadOperation: Failed to reserve space on local file.");
            return std::unexpected(Error{.type = ErrorType::OpenFailure});
        }
        localFile_.seekp(pos);
    }

    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "DownloadOperation: Download of '{}' to '{}' canceled.",
            options_.remotePath.value(),
            options_.localPath.toUtf8()
        );
        state_ = OperationState::Canceled;
    }

    cleanup();
    return {};
}

void DownloadOperation::cleanup()
{
    if (asyncTransferContext_)
    {
        asyncTransferContext_->cancel();
    }

    if (auto stream = fileStream_.lock(); stream)
        stream->close(false);

    localFile_.close();

    auto tempPath = localNativePath_;
    tempPath += options_.tempFileSuffix;
    if (options_.doCleanup && std::filesystem::exists(tempPath))
        std::filesystem::remove(tempPath);
}

void DownloadOperation::pause(bool doPause)
{
    if (asyncTransferContext_)
        asyncTransferContext_->pause(doPause);
}

std::expected<void, DownloadOperation::Error> DownloadOperation::finalize()
{
    auto fut = sftp_->performPromise(
        [this]()
        {
            return finalizeInStrand();
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: finalize umbrella timed out.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<void, DownloadOperation::Error> DownloadOperation::finalizeInStrand()
{
    if (state_ == OperationState::Running)
    {
        Log::error("DownloadOperation: Cannot finalize while reading.");
        return std::unexpected(Error{.type = ErrorType::CannotFinalizeDuringRead});
    }

    localFile_.close();

    auto tempPath = localNativePath_;
    tempPath += options_.tempFileSuffix;

    // A short file must never replace the destination.
    std::error_code sizeError{};
    const auto writtenSize = std::filesystem::file_size(tempPath, sizeError);
    if (sizeError || writtenSize != options_.entry->size)
    {
        Log::error(
            "DownloadOperation: '{}' holds {} bytes instead of {}.",
            tempPath.generic_string(),
            sizeError ? 0u : writtenSize,
            options_.entry->size
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SizeMismatch,
                .extraInfo = fmt::format(
                    "Received {} of {} bytes for {}",
                    sizeError ? 0u : writtenSize,
                    options_.entry->size,
                    options_.remotePath.value()
                ),
            }
        );
    }

    // Without permission to overwrite, the rename itself refuses an existing file: no check-then-rename race.
    std::error_code ec{};
    if (options_.mayOverwrite)
        std::filesystem::rename(tempPath, localNativePath_, ec);
    else if (auto renamed = Utility::renameNoReplace(tempPath, localNativePath_); !renamed.has_value())
        ec = renamed.error();
    if (ec == std::errc::file_exists)
    {
        Log::error(
            "DownloadOperation: File '{}' already exists and may not be overwritten.", options_.localPath.toUtf8()
        );
        return std::unexpected(Error{.type = ErrorType::FileExists});
    }
    if (ec)
    {
        Log::error("DownloadOperation: Failed to rename file: {}", ec.message());
        return std::unexpected(Error{.type = ErrorType::RenameFailure});
    }

    if (options_.inheritPermissions)
    {
        auto stream = fileStream_.lock();
        if (!stream)
        {
            Log::error("DownloadOperation: File stream expired.");
            return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
        }

        const auto fileInfo = stream->statInStrand();
        if (!fileInfo.has_value())
        {
            Log::error("DownloadOperation: Failed to stat file.");
            return std::unexpected(Error{.type = ErrorType::FileStatFailed, .sftpError = fileInfo.error()});
        }

        // Special bits never travel and the umask applies, like for a file created locally.
        std::error_code permissionsError{};
        if (const auto inherited = Utility::inheritablePermissions(fileInfo->permissions); inherited)
            std::filesystem::permissions(localNativePath_, *inherited, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }
    else if (options_.filePermissions)
    {
        std::error_code permissionsError{};
        if (const auto configured = Utility::sanitizePermissions(*options_.filePermissions); configured)
            std::filesystem::permissions(localNativePath_, *configured, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }
    /* else keep default */

    // Preserve the remote file's mtime on the downloaded local file so
    // subsequent syncs don't see the file as modified (best-effort; a failure
    // is only a warning).  Mirrors UploadOperation's post-transfer setstat.
    if (options_.entry)
    {
        using namespace std::chrono;
        const auto sysTime = system_clock::time_point{duration_cast<system_clock::duration>(
            seconds{options_.entry->mtime} + nanoseconds{options_.entry->mtimeNsec}
        )};
        const auto fileTime = std::chrono::file_clock::from_sys(sysTime);
        std::error_code mtimeError{};
        std::filesystem::last_write_time(localNativePath_, fileTime, mtimeError);
        if (mtimeError)
        {
            Log::warn(
                "DownloadOperation: Failed to set mtime on '{}': {}.", options_.localPath.toUtf8(), mtimeError.message()
            );
        }
    }
    return {};
}