#include <backend/sftp/download_operation.hpp>

#include <log/log.hpp>
#include <tuple>

DownloadOperation::DownloadOperation(
    std::weak_ptr<SecureShell::IFileStream> fileStream,
    DownloadOperationOptions options)
    : Operation{}
    , fileStream_{std::move(fileStream)}
    , remotePath_{std::move(options.remotePath)}
    , localPath_{std::move(options.localPath)}
    , tempFileSuffix_{std::move(options.tempFileSuffix)}
    , progressCallback_{std::move(options.progressCallback)}
    , mayOverwrite_{options.mayOverwrite}
    , reserveSpace_{options.reserveSpace}
    , tryContinue_{options.tryContinue}
    , inheritPermissions_{options.inheritPermissions}
    , doCleanup_{options.doCleanup}
    , permissions_{options.permissions}
    , localFile_{}
    , fileSize_{0}
    , futureTimeout_{options.futureTimeout}
{
    if (tempFileSuffix_.empty())
        tempFileSuffix_ = ".filepart";
    if (tempFileSuffix_.find('/') != 0)
        tempFileSuffix_ = ".filepart";
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
            [[fallthrough]];
        }
        case (Running):
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
                Log::info("DownloadOperation: Data reading completed.");
                state_ = Finalizing;
                [[fallthrough]];
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
    }
    Log::error("DownloadOperation: Unknown operation state: {}", static_cast<int>(state_));
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
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

    if (fileSize_ == 0)
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

    const auto futureStatus = future.wait_for(futureTimeout_);

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
        fileSize = fileSize_;
        good = localFile_.good();
        progressCallback_(0ull, fileSize, tellp);
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

std::expected<void, DownloadOperation::Error> DownloadOperation::openOrAdoptFile(SecureShell::IFileStream& stream)
{
    const auto tempPath = localPath_.generic_string() + tempFileSuffix_;

    if (tryContinue_ && std::filesystem::exists(tempPath))
    {
        localFile_.open(tempPath, std::ios::binary | std::ios::app);
        if (!localFile_.is_open())
        {
            Log::error("DownloadOperation: Failed to open file for appending: {}", tempPath);
            return enterErrorState({.type = ErrorType::OpenFailure});
        }

        // File complete but not renamed? just rename it in the finalize() step
        if (static_cast<std::uint64_t>(localFile_.tellp()) == fileSize_)
        {
            Log::info("DownloadOperation: File '{}' already complete, will be renamed in finalize() step.", tempPath);
            localFile_.close();
            return {};
        }
        // File is larger than expected? discard it and start over.
        else if (static_cast<std::uint64_t>(localFile_.tellp()) > fileSize_)
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

std::expected<void, Operation::Error> DownloadOperation::prepare()
{
    if (localPath_.empty())
    {
        Log::error("DownloadOperation: Invalid local path.");
        return enterErrorState({.type = ErrorType::InvalidPath});
    }

    // Initial check. Check again later before rename
    if (std::filesystem::exists(localPath_))
    {
        if (!mayOverwrite_)
        {
            Log::error(
                "DownloadOperation: File '{}' already exists and may not be overwritten.", localPath_.generic_string());
            return enterErrorState({.type = ErrorType::FileExists});
        }
    }

    auto stream = fileStream_.lock();
    if (!stream)
    {
        Log::error("DownloadOperation: File stream expired.");
        return enterErrorState({.type = ErrorType::FileStreamExpired});
    }

    const auto fileInfo = stream->stat().get();
    if (!fileInfo.has_value())
    {
        Log::error("DownloadOperation: Failed to stat file.");
        return enterErrorState({.type = ErrorType::FileStatFailed, .sftpError = fileInfo.error()});
    }

    fileSize_ = fileInfo->size;

    auto openResult = openOrAdoptFile(*stream);
    if (!openResult.has_value())
    {
        Log::error("DownloadOperation: Failed to open file.");
        return enterErrorState(std::move(openResult).error());
    }

    if (reserveSpace_ && fileSize_ != 0)
    {
        // Reserve space
        Log::info("DownloadOperation: Reserving space for file.");
        const auto pos = localFile_.tellp();
        localFile_.seekp(fileSize_ - 1);
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
        remotePath_.generic_string(),
        localPath_.generic_string());

    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info(
            "DownloadOperation: Download of '{}' to '{}' canceled.",
            remotePath_.generic_string(),
            localPath_.generic_string());
        state_ = OperationState::Canceled;
    }

    cleanup();
    return {};
}

void DownloadOperation::cleanup()
{
    localFile_.close();

    if (doCleanup_ && std::filesystem::exists(localPath_.generic_string() + tempFileSuffix_))
        std::filesystem::remove(localPath_.generic_string() + tempFileSuffix_);

    if (auto stream = fileStream_.lock(); stream)
        stream->close(false);
}

std::expected<void, DownloadOperation::Error> DownloadOperation::finalize()
{
    if (state_ == OperationState::Running)
    {
        Log::error("DownloadOperation: Cannot finalize while reading.");
        return std::unexpected(Error{.type = ErrorType::CannotFinalizeDuringRead});
    }

    localFile_.close();

    if (std::filesystem::exists(localPath_) && !mayOverwrite_)
    {
        Log::error(
            "DownloadOperation: File '{}' already exists and may not be overwritten.", localPath_.generic_string());
        return std::unexpected(Error{.type = ErrorType::FileExists});
    }

    std::error_code ec{};
    std::filesystem::rename(localPath_.generic_string() + tempFileSuffix_, localPath_, ec);
    if (ec)
    {
        Log::error("DownloadOperation: Failed to rename file: {}", ec.message());
        return std::unexpected(Error{.type = ErrorType::RenameFailure});
    }

    if (inheritPermissions_)
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
        std::filesystem::permissions(localPath_, fileInfo->permissions, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }
    else if (permissions_)
    {
        Log::info("DownloadOperation: Setting permissions.");
        std::error_code permissionsError{};
        std::filesystem::permissions(localPath_, *permissions_, permissionsError);
        if (permissionsError)
        {
            Log::error("DownloadOperation: Failed to set permissions: {}", permissionsError.message());
            return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
        }
    }

    Log::info(
        "DownloadOperation: Finalized download of '{}' to '{}'.",
        remotePath_.generic_string(),
        localPath_.generic_string());
    return {};
}