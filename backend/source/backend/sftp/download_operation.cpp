#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/download_operation.hpp>
#include <utility/error_message.hpp>
#include <backend/sftp/resume_check.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>

#include <log/log.hpp>
#include <utility/permissions.hpp>
#include <utility/download_mark.hpp>
#include <utility/link_safe_remove.hpp>

#include <tuple>

using namespace std::chrono_literals;

DownloadOperation::DownloadOperation(std::weak_ptr<SecureShell::SftpSession> sftp, DownloadOperationOptions options)
    : Operation{}
    , sftp_{std::move(sftp)}
    , options_{std::move(options)}
    , localFile_{}
{
    options_.tempFileSuffix = Persistence::effectiveTempFileSuffix(options_.tempFileSuffix);
    remotePath_ = options_.remotePath;
    if (!options_.localPath.empty())
    {
        localNativePath_ = options_.localPath.native();
        tempNativePath_ = localNativePath_;
        tempNativePath_ += options_.tempFileSuffix;
    }
}

DownloadOperation::~DownloadOperation()
{
    std::ignore = cancel(false);
    waitForStrandTasks(strand());
}

std::expected<DownloadOperation::WorkStatus, DownloadOperation::Error> DownloadOperation::work()
{
    // Drive one work() step from outside the strand. OperationQueue normally calls
    // workInStrand directly inside its batched umbrella, so this wrapper is only used by
    // callers that aren't already on the strand (standalone ops, tests).
    const auto sftp = openSession(sftp_);
    if (!sftp)
        return enterErrorState<WorkStatus>(channelClosedError());
    auto fut = sftp->performPromise(
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

    const auto sftp = openSession(sftp_);
    if (!sftp && !hasEnded())
        return enterErrorState<WorkStatus>(channelClosedError());

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
            if (std::error_code existsError{}; std::filesystem::exists(localNativePath_, existsError) && !options_.mayOverwrite)
            {
                Log::error(
                    "DownloadOperation: File '{}' already exists and may not be overwritten.",
                    options_.localPath.toUtf8()
                );
                return enterErrorState<WorkStatus>({.type = ErrorType::FileExists});
            }
            const auto prepareResult = prepareInStrand(*sftp);
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
                const auto result = handleSymlinkInStrand(*sftp);
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
                observeTransfer(*asyncTransferContext_);
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
                    return WorkStatus::Waiting;
                }
            }
        }
        case (Finalizing):
        {
            const auto finalizeResult = finalizeInStrand();
            if (!finalizeResult.has_value())
            {
                Log::log(
                    failureLevelOf(finalizeResult.error()),
                    "DownloadOperation: Failed to finalize operation: {}",
                    finalizeResult.error().toString()
                );
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
    const auto sftp = openSession(sftp_);
    if (!sftp)
        return std::unexpected(channelClosedError());
    auto fut = sftp->performPromise(
        [this]() -> std::expected<void, Error>
        {
            const auto sftp = openSession(sftp_);
            if (!sftp)
                return std::unexpected(channelClosedError());
            return handleSymlinkInStrand(*sftp);
        }
    );
    if (fut.wait_for(options_.futureTimeout) != std::future_status::ready)
    {
        Log::error("DownloadOperation: handleSymlink umbrella timed out.");
        return std::unexpected(Error{.type = ErrorType::FutureTimeout});
    }
    return fut.get();
}

std::expected<void, DownloadOperation::Error> DownloadOperation::handleSymlinkInStrand(SecureShell::SftpSession& sftp)
{
    if (options_.symlinkHandling == Persistence::SymlinkHandling::SkipSymlink)
    {
        Log::info("DownloadOperation: Skipping symlink as per options: {}.", options_.remotePath.value());
        return {};
    }

    const auto readResult = readSymlinkInStrand(sftp, remotePath_);
    if (!readResult.has_value())
        return std::unexpected(readResult.error());
    auto const& linkEntry = readResult.value().targetInfo;

    // An existing link at the leaf makes creating the new one fail, only the directories above it matter.
    if (!options_.linkFreeRoot.empty())
    {
        if (const auto refused = refuseLinkedPath(localNativePath_.parent_path()); !refused)
            return refused;
    }

    std::error_code ec{};
    if (options_.createMissingDirectories)
    {
        std::filesystem::create_directories(localNativePath_.parent_path(), ec);
        if (ec)
        {
            Log::error(
                "DownloadOperation: Failed to create parent dirs for symlink '{}': {}.",
                options_.localPath.toUtf8(),
                Utility::errorMessage(ec)
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }
    // The link is created under the temp name and moved into place like a file. Its copy keeps the link's own
    // content, so a relative link still points into the downloaded tree.
    auto const& linkText = readResult.value().linkContent;
    if (!Paths::isValidUtf8(linkText))
    {
        Log::error("DownloadOperation: The link '{}' points to a name that is not UTF-8.", options_.remotePath.value());
        return std::unexpected(
            Error{.type = ErrorType::InvalidName, .extraInfo = Paths::sanitizedUtf8(linkText)}
        );
    }
    std::filesystem::remove(tempNativePath_, ec);
    ec.clear();
    const auto linkContent = Paths::nativeFromUtf8(linkText);
    if (linkEntry && linkEntry->isDirectory())
        std::filesystem::create_directory_symlink(linkContent, tempNativePath_, ec);
    else
        std::filesystem::create_symlink(linkContent, tempNativePath_, ec);
    if (ec)
    {
        Log::error(
            "DownloadOperation: Failed to create symlink at '{}': {}.",
            Paths::nativeToUtf8(tempNativePath_),
            Utility::errorMessage(ec)
        );
        return std::unexpected(Error{.type = ErrorType::CannotCreateSymlink});
    }

    if (auto moved = ComparedStateCheck::moveLocalIntoPlace(
            tempNativePath_, localNativePath_, options_.mayOverwrite, options_.comparedDestination
        );
        !moved)
    {
        std::filesystem::remove(tempNativePath_, ec);
        return std::unexpected(std::move(moved).error());
    }
    return {};
}

std::expected<bool, DownloadOperation::Error> DownloadOperation::readOnce()
{
    const auto sftp = openSession(sftp_);
    if (!sftp)
        return enterErrorState<bool>(channelClosedError());
    auto fut = sftp->performPromise(
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
    const auto fileSize = options_.entry->size;

    if (readAmount == 0)
    {
        if (bytesInFile_ >= fileSize)
            return false;
        Log::error("DownloadOperation: '{}' ended after {} of {} bytes.", options_.remotePath.value(), bytesInFile_, fileSize);
        return enterErrorState<bool>(sizeMismatch());
    }

    localFile_.write(buffer_.data(), static_cast<std::streamsize>(readAmount));
    bytesInFile_ += static_cast<std::uint64_t>(readAmount);
    const bool good = localFile_.good();
    options_.progressCallback(0ull, fileSize, bytesInFile_, 0);
    if (!good)
    {
        Log::error("DownloadOperation read cycle stopped: localFile_.good() == false");
        std::ignore = enterErrorState({
            .type = SharedData::OperationErrorType::TargetFileNotGood,
        });
        return false;
    }
    return bytesInFile_ < fileSize;
}

DownloadOperation::Error DownloadOperation::sizeMismatch() const
{
    return Error{
        .type = ErrorType::SizeMismatch,
        .extraInfo = fmt::format(
            "Received {} of {} bytes for {}", bytesInFile_, options_.entry->size, options_.remotePath.value()
        ),
    };
}

bool DownloadOperation::commitBufferToFile(SecureShell::IFileStream::SignedSizeType bytesRead)
{
    localFile_.write(buffer_.data(), static_cast<std::streamsize>(bytesRead));
    bytesInFile_ += static_cast<std::uint64_t>(bytesRead);
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
DownloadOperation::refuseLinkedPath(std::filesystem::path const& path) const
{
    std::error_code error{};
    std::optional<std::filesystem::path> link{};
    if (!options_.linkFreeRoot.empty())
        link = Utility::firstLinkBelow(
            options_.linkFreeRoot.native(), path, error, options_.verifiedDirectories.get()
        );
    else if (Utility::isLinkOrJunction(path, error))
        link = path;
    if (error)
    {
        Log::error(
            "DownloadOperation: Cannot inspect '{}': {}.", Paths::nativeToUtf8(path), Utility::errorMessage(error)
        );
        return std::unexpected(Error{.type = ErrorType::FileStatFailed, .extraInfo = Paths::nativeToUtf8(path)});
    }
    if (link)
    {
        Log::error("DownloadOperation: '{}' runs through the link '{}'.", Paths::nativeToUtf8(path), Paths::nativeToUtf8(*link));
        return std::unexpected(
            Error{
                .type = ErrorType::LinkInLocalPath,
                .extraInfo = fmt::format("The local path runs through a link: {}", Paths::nativeToUtf8(*link))
            }
        );
    }
    return {};
}

std::expected<void, DownloadOperation::Error>
DownloadOperation::openOrAdoptFileInStrand(SecureShell::IFileStream& stream)
{
    auto const& tempPath = tempNativePath_;

    // Opening for writing follows a link planted at the temp path or above it, out of the target directory.
    if (const auto refused = refuseLinkedPath(tempPath); !refused)
        return refused;

    if (options_.createMissingDirectories)
    {
        std::error_code mkErr{};
        std::filesystem::create_directories(tempPath.parent_path(), mkErr);
        if (mkErr)
        {
            Log::error(
                "DownloadOperation: Failed to create parent directories for '{}': {}.",
                Paths::nativeToUtf8(tempPath),
                Utility::errorMessage(mkErr)
            );
            return std::unexpected(Error{.type = ErrorType::CannotCreateDirectory});
        }
    }

    std::error_code existsError{};
    if (options_.tryContinue && std::filesystem::exists(tempPath, existsError))
    {
        std::error_code sizeError{};
        const auto existingSize = std::filesystem::file_size(tempPath, sizeError);
        if (sizeError)
        {
            Log::error(
                "DownloadOperation: Cannot read the size of '{}': {}",
                Paths::nativeToUtf8(tempPath),
                Utility::errorMessage(sizeError)
            );
            return std::unexpected(Error{.type = ErrorType::FileStatFailed});
        }

        // A leftover of another program or of an older version of the source must not be continued.
        const auto belongsToSource = [&]() -> std::expected<bool, Error>
        {
            if (existingSize == 0 || existingSize > options_.entry->size)
                return false;
            return ResumeCheck::tailsMatchInStrand(stream, tempPath, existingSize)
                .transform_error(
                    [](SecureShell::SftpError error)
                    {
                        return Error{.type = ErrorType::SftpError, .sftpError = std::move(error)};
                    }
                );
        }();
        if (!belongsToSource)
            return std::unexpected(belongsToSource.error());

        if (*belongsToSource && existingSize == options_.entry->size)
        {
            Log::info(
                "DownloadOperation: File '{}' already complete, will be renamed in finalize() step.",
                Paths::nativeToUtf8(tempPath)
            );
            bytesInFile_ = existingSize;
            transferComplete_ = true;
            return {};
        }
        else if (!*belongsToSource)
        {
            Log::info(
                "DownloadOperation: File '{}' is not a part of this source, discarding and starting over.",
                Paths::nativeToUtf8(tempPath)
            );
            if (auto rewound = stream.seekInStrand(0); !rewound)
                return std::unexpected(Error{.type = ErrorType::SftpError, .sftpError = rewound.error()});
            if (const auto opened = openEmptyTempFile(); !opened)
                return opened;
        }
        else
        {
            Log::info("DownloadOperation: File '{}' is incomplete, continuing download.", Paths::nativeToUtf8(tempPath));
            if (const auto restricted = restrictTempFile(); !restricted)
                return restricted;
            localFile_.open(tempPath, std::ios::binary | std::ios::app);
            if (!localFile_.is_open())
            {
                Log::error("DownloadOperation: Failed to open file for appending: {}", Paths::nativeToUtf8(tempPath));
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
        if (const auto opened = openEmptyTempFile(); !opened)
            return opened;
    }

    if (!localFile_.is_open())
    {
        Log::error("DownloadOperation: Failed to open file: {}", Paths::nativeToUtf8(tempPath));
        return std::unexpected(Error{.type = ErrorType::OpenFailure});
    }

    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::restrictTempFile() const
{
    if (const auto restricted = Utility::restrictToOwner(tempNativePath_); !restricted)
    {
        Log::error(
            "DownloadOperation: Cannot create '{}': {}",
            Paths::nativeToUtf8(tempNativePath_),
            Utility::errorMessage(restricted.error())
        );
        return std::unexpected(Error{.type = ErrorType::OpenFailure});
    }
    return {};
}

std::expected<void, DownloadOperation::Error> DownloadOperation::openEmptyTempFile()
{
    if (const auto restricted = restrictTempFile(); !restricted)
        return restricted;

    // Opening with trunc truncates the file restrictTempFile just created, and on ext4 a truncated file is written
    // back in full when it is closed (auto_da_alloc), which took 160 ms of a 400 ms download of 256 MiB. Only a
    // leftover with content is truncated. On Windows restrictTempFile creates nothing, the open creates the file.
    std::error_code fileError{};
    const bool exists = std::filesystem::exists(tempNativePath_, fileError);
    if (exists && !fileError)
    {
        const auto existingSize = std::filesystem::file_size(tempNativePath_, fileError);
        if (!fileError && existingSize != 0)
            std::filesystem::resize_file(tempNativePath_, 0, fileError);
    }
    if (fileError)
    {
        Log::error(
            "DownloadOperation: Cannot empty '{}': {}",
            Paths::nativeToUtf8(tempNativePath_),
            Utility::errorMessage(fileError)
        );
        return std::unexpected(Error{.type = ErrorType::OpenFailure});
    }
    // Without in, out truncates, which only matters for an existing file.
    localFile_.open(
        tempNativePath_, exists ? std::ios::binary | std::ios::in | std::ios::out : std::ios::binary | std::ios::out
    );
    return {};
}

std::expected<SecureShell::SftpSession::DeepLinkResult, DownloadOperation::Error>
DownloadOperation::readSymlinkInStrand(SecureShell::SftpSession& sftp, Paths::RemotePath const& remoteFullPath)
{
    auto deepLinkResult = sftp.readLinkDeepInStrand(remoteFullPath);
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

    if (std::error_code existsError{}; std::filesystem::exists(localNativePath_, existsError))
    {
        if (!options_.mayOverwrite)
        {
            Log::error(
                "DownloadOperation: File '{}' already exists and may not be overwritten.", options_.localPath.toUtf8()
            );
            return enterErrorState({.type = ErrorType::FileExists});
        }
    }

    const auto sftp = openSession(sftp_);
    if (!sftp)
        return enterErrorState(channelClosedError());
    auto fut = sftp->performPromise(
        [this]() -> std::expected<void, Error>
        {
            const auto sftp = openSession(sftp_);
            if (!sftp)
                return std::unexpected(channelClosedError());
            return prepareInStrand(*sftp);
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

std::expected<void, DownloadOperation::Error> DownloadOperation::prepareInStrand(SecureShell::SftpSession& sftp)
{
    if (remotePath_.empty() || remotePath_.isRoot())
    {
        Log::error("DownloadOperation: Invalid remote path '{}'.", options_.remotePath.value());
        return std::unexpected(Error{.type = ErrorType::InvalidPath, .extraInfo = options_.remotePath.value()});
    }

    if (!options_.entry)
    {
        const auto fileInfo = sftp.lstatInStrand(remotePath_);
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

    const auto streamOpenResult =
        sftp.openFileInStrand(remotePath_, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown);
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

    buffer_ = sftp.bufferProvider().leaseForTransfer(options_.entry->size, stream->readLengthLimit());
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

    // Appending to a partial file or finishing a complete one reserves nothing.
    if (options_.reserveSpace && options_.entry->size != 0 && resumeOffset_ == 0 && !transferComplete_)
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

    if (options_.doCleanup && !tempNativePath_.empty())
    {
        std::error_code removeError{};
        std::filesystem::remove(tempNativePath_, removeError);
        if (removeError)
            Log::warn(
                "DownloadOperation: Cannot remove '{}': {}",
                Paths::nativeToUtf8(tempNativePath_),
                Utility::errorMessage(removeError)
            );
    }
}

void DownloadOperation::pause(bool doPause)
{
    if (asyncTransferContext_)
        asyncTransferContext_->pause(doPause);
}

std::expected<void, DownloadOperation::Error> DownloadOperation::finalize()
{
    const auto sftp = openSession(sftp_);
    if (!sftp)
        return std::unexpected(channelClosedError());
    auto fut = sftp->performPromise(
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

    auto const& tempPath = tempNativePath_;

    // A short file must never replace the destination. A reserved file has its full size from the start, so the
    // bytes written count.
    std::error_code sizeError{};
    const auto sizeOnDisk = std::filesystem::file_size(tempPath, sizeError);
    if (sizeError || sizeOnDisk != options_.entry->size || bytesInFile_ != options_.entry->size)
    {
        Log::error(
            "DownloadOperation: '{}' received {} bytes and holds {} instead of {}.",
            Paths::nativeToUtf8(tempPath),
            bytesInFile_,
            sizeError ? 0u : sizeOnDisk,
            options_.entry->size
        );
        return std::unexpected(sizeMismatch());
    }

    // Marked before the rename, so the final file never exists without the mark. A filesystem without streams or
    // extended attributes, FAT for one, cannot carry it.
    if (options_.markAsDownloaded)
    {
        if (const auto marked = Utility::markAsDownloaded(tempPath); marked)
            Log::warn(
                "DownloadOperation: Cannot mark '{}' as downloaded: {}",
                Paths::nativeToUtf8(tempPath),
                Utility::errorMessage(marked)
            );
    }

    // Times and permissions go on the temp file, so nothing can fail once the destination is replaced.
    applyModificationTime(tempPath);
    const auto permissions = finalPermissionsInStrand();
    if (!permissions)
        return std::unexpected(permissions.error());
    std::error_code permissionsError{};
    std::filesystem::permissions(tempPath, *permissions, permissionsError);
    if (permissionsError)
    {
        Log::error("DownloadOperation: Failed to set permissions: {}", Utility::errorMessage(permissionsError));
        return std::unexpected(Error{.type = ErrorType::CannotSetFilePermissions});
    }

    return ComparedStateCheck::moveLocalIntoPlace(
        tempPath, localNativePath_, options_.mayOverwrite, options_.comparedDestination
    );
}

std::expected<std::filesystem::perms, DownloadOperation::Error> DownloadOperation::finalPermissionsInStrand()
{
    if (!options_.inheritPermissions)
    {
        return options_.filePermissions.and_then(Utility::sanitizePermissions).value_or(Utility::newFilePermissions());
    }

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
    return Utility::inheritablePermissions(fileInfo->permissions).value_or(Utility::newFilePermissions());
}

void DownloadOperation::applyModificationTime(std::filesystem::path const& path) const
{
    using namespace std::chrono;
    const auto sysTime = system_clock::time_point{
        duration_cast<system_clock::duration>(seconds{options_.entry->mtime} + nanoseconds{options_.entry->mtimeNsec})
    };
    std::error_code mtimeError{};
    std::filesystem::last_write_time(path, std::chrono::file_clock::from_sys(sysTime), mtimeError);
    if (mtimeError)
    {
        Log::warn(
            "DownloadOperation: Failed to set mtime on '{}': {}.",
            Paths::nativeToUtf8(path),
            Utility::errorMessage(mtimeError)
        );
    }
}