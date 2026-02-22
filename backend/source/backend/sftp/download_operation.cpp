#include <backend/sftp/download_operation.hpp>

#include <log/log.hpp>
#include <tuple>

using namespace std::chrono_literals;

DownloadOperation::DownloadOperation(SecureShell::SftpSession& sftp, DownloadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
    , localFile_{}
{
    if (options_.tempFileSuffix.empty())
        options_.tempFileSuffix = ".filepart";
    if (options_.tempFileSuffix.find('/') != 0)
        options_.tempFileSuffix = ".filepart";
}

DownloadOperation::~DownloadOperation()
{
    std::ignore = cancel(false);

    if (auto stream = fileStream_.lock(); stream)
    {
        // wait for all tasks of the operation to finish
        stream->strand()->pushPromiseTask([]() {}).get();
    }
}

std::expected<DownloadOperation::WorkStatus, DownloadOperation::Error> DownloadOperation::work()
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
                const auto result = handleSymlink();
                if (!result.has_value())
                {
                    Log::error("DownloadOperation: Failed to handle symlink: {}", result.error().toString());
                    return enterErrorState<WorkStatus>(result.error());
                }
                Log::info("DownloadOperation: Symlink handled, no file to download.");
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
                    Log::error("DownloadOperation: File stream expired.");
                    return enterErrorState<WorkStatus>({.type = ErrorType::FileStreamExpired});
                }
                auto contextFut = stream->readAsync(
                    options_.entry->size,
                    buffer_.data(),
                    buffer_.size(),
                    [this](SecureShell::IFileStream::SignedSizeType bytesRead)
                    {
                        return commitBufferToFile(bytesRead);
                    }
                );
                const auto futureStatus = contextFut.wait_for(options_.futureTimeout);
                if (futureStatus != std::future_status::ready)
                {
                    Log::error("DownloadOperation: Future timed out while starting async read.");
                    return enterErrorState<WorkStatus>({.type = ErrorType::FutureTimeout});
                }
                const auto contextResult = contextFut.get();
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
                const auto result = readOnce();
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
                    state_ = Finalizing;
                    [[fallthrough]];
                }
            }
            else
            {
                if (asyncTransferContext_->hasEnded())
                {
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
                Log::error("DownloadOperation: Failed to finalize operation: {}", finalizeResult.error().toString());
                return enterErrorState<WorkStatus>(finalizeResult.error());
            }
            state_ = Completed;
            Log::info("DownloadOperation: Operation completed successfully.");
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
    if (options_.symlinkHandling == Persistence::SymlinkHandling::SkipSymlink)
    {
        Log::info("DownloadOperation: Skipping symlink as per options: {}.", options_.remotePath.string());
        return {};
    }

    const auto readResult = readSymlink(options_.remotePath);
    if (!readResult.has_value())
        return std::unexpected(readResult.error());
    auto const& linkEntry = readResult.value().targetInfo;

    std::error_code ec{};
    if (linkEntry && linkEntry->isDirectory())
        std::filesystem::create_directory_symlink(readResult.value().linkTarget, options_.localPath, ec);
    else
        std::filesystem::create_symlink(readResult.value().linkTarget, options_.localPath, ec);
    if (ec)
    {
        Log::error(
            "DownloadOperation: Failed to create symlink at '{}': {}.",
            options_.localPath.generic_string(),
            ec.message()
        );
        return std::unexpected(Error{.type = ErrorType::CannotCreateSymlink});
    }
    return {};
}

std::expected<bool, DownloadOperation::Error> DownloadOperation::readOnce()
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
        Log::info("DownloadOperation: Remote file is empty, nothing to do.");
        return false;
    }

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("DownloadOperation: File stream expired.");
        return enterErrorState<bool>({.type = ErrorType::FileStreamExpired});
    }

    auto future = stream->readSome(buffer_.data(), buffer_.size());

    const auto futureStatus = future.wait_for(options_.futureTimeout);

    if (futureStatus != std::future_status::ready)
    {
        Log::error("DownloadOperation: Future timed out while reading.");
        return enterErrorState<bool>({.type = ErrorType::FutureTimeout});
    }

    const auto result = future.get();

    if (!result.has_value())
    {
        Log::error("DownloadOperation: Failed to read from remote file: {}", result.error().message);
        return enterErrorState<bool>({.type = ErrorType::SftpError, .sftpError = result.error()});
    }

    const auto readAmount = result.value();

    if (readAmount == 0)
    {
        Log::info("DownloadOperation: Remote file read complete or error.");
        return false;
    }

    std::uint64_t tellp = 0;
    std::uint64_t fileSize = 0;
    bool good = true;
    {
        localFile_.write(buffer_.data(), static_cast<std::streamsize>(readAmount));
        tellp = static_cast<uint64_t>(localFile_.tellp());
        fileSize = options_.entry->size;
        good = localFile_.good();
        options_.progressCallback(0ull, fileSize, tellp, 0);
    }
    if (!good)
    {
        Log::error("DownloadOperation read cycle stopped: localFile_.good() == false");
        std::ignore = enterErrorState({
            .type = SharedData::OperationErrorType::TargetFileNotGood,
        });
        return false;
    }
    return good && tellp < fileSize;
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

std::expected<void, DownloadOperation::Error> DownloadOperation::openOrAdoptFile(SecureShell::IFileStream& stream)
{
    const auto tempPath = options_.localPath.generic_string() + options_.tempFileSuffix;

    if (options_.tryContinue && std::filesystem::exists(tempPath))
    {
        localFile_.open(tempPath, std::ios::binary | std::ios::app);
        if (!localFile_.is_open())
        {
            Log::error("DownloadOperation: Failed to open file for appending: {}", tempPath);
            return enterErrorState({.type = ErrorType::OpenFailure});
        }

        // File complete but not renamed? just rename it in the finalize() step
        if (static_cast<std::uint64_t>(localFile_.tellp()) == options_.entry->size)
        {
            Log::info("DownloadOperation: File '{}' already complete, will be renamed in finalize() step.", tempPath);
            localFile_.close();
            return {};
        }
        // File is larger than expected? discard it and start over.
        else if (static_cast<std::uint64_t>(localFile_.tellp()) > options_.entry->size)
        {
            Log::info("DownloadOperation: File '{}' is larger than expected, discarding and starting over.", tempPath);
            localFile_.close();
            // Reset the file
            localFile_.open(tempPath, std::ios::binary | std::ios::trunc);
        }
        else
        {
            Log::info("DownloadOperation: File '{}' is incomplete, continuing download.", tempPath);
            // Seek stream to position:
            auto seekResult = stream.seek(localFile_.tellp()).get();
            if (!seekResult.has_value())
                return enterErrorState({.type = ErrorType::FileStatFailed});
        }
    }
    else
    {
        Log::info("DownloadOperation: Starting new download to '{}'.", tempPath);
        localFile_.open(tempPath, std::ios::binary | std::ios::trunc);
    }

    if (!localFile_.is_open())
    {
        Log::error("DownloadOperation: Failed to open file: {}", tempPath);
        return enterErrorState({.type = ErrorType::OpenFailure});
    }

    return {};
}

std::expected<SecureShell::SftpSession::DeepLinkResult, DownloadOperation::Error>
DownloadOperation::readSymlink(std::filesystem::path const& remoteFullPath)
{
    auto targetResult = sftp_->readLinkDeep(remoteFullPath);
    const auto status = targetResult.wait_for(options_.futureTimeout);
    if (status != std::future_status::ready)
    {
        Log::error(
            "DownloadOperation: Failed to read symlink target: timeout for entry: {}.", remoteFullPath.generic_string()
        );
        return std::unexpected(
            Error{
                .type = ErrorType::FutureTimeout,
                .extraInfo =
                    fmt::format("Timeout reading symlink target for entry: {}", remoteFullPath.generic_string())
            }
        );
    }
    const auto deepLinkResult = targetResult.get();
    if (!deepLinkResult.has_value())
    {
        Log::error(
            "DownloadOperation: Failed to read symlink target: {} for entry: {}.",
            deepLinkResult.error().toString(),
            remoteFullPath.generic_string()
        );
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = deepLinkResult.error(),
                .extraInfo = fmt::format("Reading symlink target for entry: {}", remoteFullPath.generic_string())
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

    // Initial check. Check again later before rename
    if (std::filesystem::exists(options_.localPath))
    {
        if (!options_.mayOverwrite)
        {
            Log::error(
                "DownloadOperation: File '{}' already exists and may not be overwritten.",
                options_.localPath.generic_string()
            );
            return enterErrorState({.type = ErrorType::FileExists});
        }
    }

    if (!options_.entry)
    {
        auto fut = sftp_->lstat(options_.remotePath);
        const auto status = fut.wait_for(options_.futureTimeout);
        if (status != std::future_status::ready)
        {
            Log::error("DownloadOperation: Failed to stat file: timeout.");
            return enterErrorState({.type = ErrorType::FutureTimeout, .extraInfo = "Timeout stating file"});
        }
        const auto fileInfo = fut.get();
        if (!fileInfo.has_value())
        {
            Log::error("DownloadOperation: Failed to stat file.");
            return enterErrorState({.type = ErrorType::FileStatFailed, .sftpError = fileInfo.error()});
        }
        options_.entry = std::move(fileInfo).value();
    }

    // Dont try to open symlink as file, unless option says to download said file as a file:
    if (options_.entry->isSymlink() && options_.symlinkHandling != Persistence::SymlinkHandling::FollowSymlink)
        return {};

    auto fileFut =
        sftp_->openFile(options_.remotePath, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown);

    if (fileFut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("BulkDownloadOperation: Failed to open remote sftp file: timeout.");
        return std::unexpected(
            Error{
                .type = ErrorType::FutureTimeout,
                .extraInfo = fmt::format("Timeout opening remote file: {}", options_.remotePath.string())
            }
        );
    }

    const auto streamOpenResult = fileFut.get();
    if (!streamOpenResult.has_value())
    {
        Log::error("BulkDownloadOperation: Failed to open remote sftp file: {}.", streamOpenResult.error().message);
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = streamOpenResult.error(),
                .extraInfo = fmt::format("Opening remote file: {}", options_.remotePath.string())
            }
        );
    }

    fileStream_ = std::move(streamOpenResult).value();
    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("DownloadOperation: File stream expired after opening.");
        return enterErrorState({.type = ErrorType::FileStreamExpired});
    }

    auto openResult = openOrAdoptFile(*stream);
    if (!openResult.has_value())
    {
        Log::error("DownloadOperation: Failed to open file.");
        return enterErrorState(std::move(openResult).error());
    }

    if (options_.entry->isSymlink())
        return {};

    if (options_.reserveSpace && options_.entry->size != 0)
    {
        // Reserve space
        Log::info("DownloadOperation: Reserving space for file.");
        const auto pos = localFile_.tellp();
        localFile_.seekp(options_.entry->size - 1);
        localFile_.put('\0');
        if (localFile_.fail())
        {
            Log::error("DownloadOperation: Failed to open file.");
            return enterErrorState({.type = ErrorType::OpenFailure});
        }
        localFile_.seekp(pos);
    }

    Log::info(
        "DownloadOperation: Prepared download of '{}' to '{}'.",
        options_.remotePath.generic_string(),
        options_.localPath.generic_string()
    );

    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "DownloadOperation: Download of '{}' to '{}' canceled.",
            options_.remotePath.generic_string(),
            options_.localPath.generic_string()
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

    if (options_.doCleanup && std::filesystem::exists(options_.localPath.generic_string() + options_.tempFileSuffix))
        std::filesystem::remove(options_.localPath.generic_string() + options_.tempFileSuffix);
}

void DownloadOperation::pause(bool doPause)
{
    if (asyncTransferContext_)
        asyncTransferContext_->pause(doPause);
}

std::expected<void, DownloadOperation::Error> DownloadOperation::finalize()
{
    if (state_ == OperationState::Running)
    {
        Log::error("DownloadOperation: Cannot finalize while reading.");
        return std::unexpected(Error{.type = ErrorType::CannotFinalizeDuringRead});
    }

    localFile_.close();

    if (std::filesystem::exists(options_.localPath) && !options_.mayOverwrite)
    {
        Log::error(
            "DownloadOperation: File '{}' already exists and may not be overwritten.",
            options_.localPath.generic_string()
        );
        return std::unexpected(Error{.type = ErrorType::FileExists});
    }

    std::error_code ec{};
    std::filesystem::rename(options_.localPath.generic_string() + options_.tempFileSuffix, options_.localPath, ec);
    if (ec)
    {
        Log::error("DownloadOperation: Failed to rename file: {}", ec.message());
        return std::unexpected(Error{.type = ErrorType::RenameFailure});
    }

    if (options_.inheritPermissions)
    {
        Log::info("DownloadOperation: Inheriting permissions from remote file.");
        auto stream = fileStream_.lock();
        if (!stream)
        {
            Log::error("DownloadOperation: File stream expired.");
            return std::unexpected(Error{.type = ErrorType::FileStreamExpired});
        }

        const auto fileInfo = stream->stat().get();
        if (!fileInfo.has_value())
        {
            Log::error("DownloadOperation: Failed to stat file.");
            return std::unexpected(Error{.type = ErrorType::FileStatFailed, .sftpError = fileInfo.error()});
        }

        std::error_code permissionsError{};
        std::filesystem::permissions(options_.localPath, fileInfo->permissions, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }
    else if (options_.filePermissions)
    {
        Log::info("DownloadOperation: Setting permissions.");
        std::error_code permissionsError{};
        std::filesystem::permissions(options_.localPath, *options_.filePermissions, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }
    /* else keep default */

    Log::info(
        "DownloadOperation: Finalized download of '{}' to '{}'.",
        options_.remotePath.generic_string(),
        options_.localPath.generic_string()
    );
    return {};
}