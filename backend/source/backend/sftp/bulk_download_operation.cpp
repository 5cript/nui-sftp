#include <backend/sftp/bulk_download_operation.hpp>
#include <ssh/sftp_session.hpp>
#include <constants/sftp.hpp>
#include <log/log.hpp>

BulkDownloadOperation::BulkDownloadOperation(SecureShell::SftpSession& sftp, BulkDownloadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
    , currentDownload_{nullptr}
    , entries_{}
    , totalBytes_{0}
    , currentIndex_{0}
    , currentBytes_{0}
    , futureTimeout_{options_.individualOptions.futureTimeout}
{}

BulkDownloadOperation::~BulkDownloadOperation() = default;

std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> BulkDownloadOperation::work()
{
    if (options_.asArchive)
        return workAsArchive();
    else
        return workNormal();
}

std::filesystem::path BulkDownloadOperation::fullLocalPath(SharedData::DirectoryEntry const& entry) const
{
    return (options_.localPath / SharedData::fullPathRelative(entries_, entry)).lexically_normal();
}

std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> BulkDownloadOperation::workNormal()
{
    using enum OperationState;

    switch (state())
    {
        case (NotStarted):
        {
            if (entries_.empty())
            {
                Log::info("BulkDownloadOperation: No entries to download.");
                enterState(Completed);
                return WorkStatus::Complete;
            }
            auto const& entry = entries_[0];
            if (entry.isDirectory())
            {
                // Base directory of the download, everything after this will be relative to this:
                const auto path = options_.localPath;
                std::error_code ec;
                std::filesystem::create_directories(path, ec);
                if (ec)
                {
                    Log::error(
                        "BulkDownloadOperation: Failed to create local directory: {}: {}", path.string(), ec.message()
                    );
                    return enterErrorState<BulkDownloadOperation::WorkStatus>(Error{
                        .type = ErrorType::CannotCreateDirectory,
                        .extraInfo = fmt::format("Creating local directory: {}: {}", path.string(), ec.message())
                    });
                }
                auto result = applyPermsToDirectory(path, entry);
                if (!result.has_value())
                    return enterErrorState<BulkDownloadOperation::WorkStatus>(result.error());
            }
            else
            {
                Log::error("BulkDownloadOperation: First entry is not a directory: {}.", entry.path.string());
                return enterErrorState<BulkDownloadOperation::WorkStatus>(
                    Error{.type = ErrorType::ImplementationError, .extraInfo = "First entry must be a directory."}
                );
            }
            currentIndex_ = 1;
            enterState(Running);
            options_.overallProgressCallback(
                options_.localPath, currentIndex_, entries_.size() - 1, 0, 0, 0, totalBytes_, 0 /*TODO: proper bps*/
            );
            return WorkStatus::MoreWork;
        }
        case (Preparing):
            [[fallthrough]];
        case (Prepared):
            [[fallthrough]];
        case (Running):
        {
            if (currentIndex_ == entries_.size())
            {
                Log::info("BulkDownloadOperation: Bulk download completed.");
                enterState(Completed);
                return WorkStatus::Complete;
            }
            if (currentIndex_ > entries_.size())
            {
                Log::error("BulkDownloadOperation: Current index out of range.");
                return enterErrorState<BulkDownloadOperation::WorkStatus>(Error{
                    .type = ErrorType::ImplementationError,
                    .extraInfo = "Bulk download index is beyond the item count, which should never occur."
                });
            }

            auto const& entry = entries_[currentIndex_];
            if (entry.isDirectory())
            {
                // Create directory:
                const auto path = fullLocalPath(entry);
                std::error_code ec;
                std::filesystem::create_directories(path, ec);
                if (ec)
                {
                    Log::error(
                        "BulkDownloadOperation: Failed to create local directory: {}: {}", path.string(), ec.message()
                    );
                    return enterErrorState<BulkDownloadOperation::WorkStatus>(Error{
                        .type = ErrorType::CannotCreateDirectory,
                        .extraInfo = fmt::format("Creating local directory: {}: {}", path.string(), ec.message())
                    });
                }
                auto result = applyPermsToDirectory(path, entry);
                if (!result.has_value())
                    return enterErrorState<BulkDownloadOperation::WorkStatus>(result.error());
                ++currentIndex_;
                options_.overallProgressCallback(
                    path, currentIndex_, entries_.size() - 1, 0, 0, currentBytes_, totalBytes_, 0 /*TODO: proper bps*/
                );
            }
            else if (entry.isRegularFile() || entry.isSymlink())
            {
                if (!currentDownload_)
                {
                    const auto remoteFullPath = SharedData::fullPath(entries_, entry);

                    auto downloadOptions = options_.individualOptions;
                    downloadOptions.remotePath = remoteFullPath;
                    downloadOptions.localPath = fullLocalPath(entry);
                    downloadOptions.bigFileOptimized = entry.size >= Constants::bigFileCutOff;

                    downloadOptions.progressCallback = [this, operationId = this->id(), remoteFullPath](
                                                           auto, auto max, auto current, auto bytesPerSecond
                                                       )
                    {
                        // Call overall progress callback
                        options_.overallProgressCallback(
                            remoteFullPath,
                            currentIndex_,
                            entries_.size() - 1,
                            current,
                            max,
                            currentBytes_ + current,
                            totalBytes_,
                            bytesPerSecond
                        );
                    };
                    downloadOptions.symlinkHandling = options_.individualOptions.symlinkHandling;
                    downloadOptions.entry = entry;

                    currentDownload_ = std::make_unique<DownloadOperation>(*sftp_, downloadOptions);
                }
                else
                {
                    return workCurrentFile();
                }
            }
            else
            {
                Log::warn(
                    "BulkDownloadOperation: Skipping unsupported file type for entry: {}.",
                    fullLocalPath(entry).string()
                );
                ++currentIndex_;
            }
            return WorkStatus::MoreWork;
        }
        case (Finalizing):
        {
            enterState(Completed);
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
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
        case (PartialSuccess):
        {
            Log::warn("DownloadOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::applyPermsToDirectory(
    std::filesystem::path const& path,
    SharedData::DirectoryEntry const& entryToInheritFrom
)
{
    std::optional<std::filesystem::perms> toApplyPerms{std::nullopt};
    if (options_.individualOptions.inheritPermissions)
    {
        toApplyPerms = entryToInheritFrom.permissions;
    }
    else if (options_.individualOptions.directoryPermissions)
    {
        toApplyPerms = options_.individualOptions.directoryPermissions;
    }

    if (toApplyPerms)
    {
        std::error_code ec;
        std::filesystem::permissions(path, *toApplyPerms, ec);
        if (ec)
        {
            Log::error(
                "BulkDownloadOperation: Failed to set permissions on local directory: {}: {}",
                path.string(),
                ec.message()
            );
            return std::unexpected(
                Error{
                    .type = ErrorType::CannotSetFilePermissions,
                    .extraInfo =
                        fmt::format("Setting permissions on local directory: {}: {}", path.string(), ec.message())
                }
            );
        }
    }
    return {};
}

std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> BulkDownloadOperation::workCurrentFile()
{
    auto result = currentDownload_->work();
    if (!result)
    {
        auto const& entry = entries_[currentIndex_];
        const auto error = result.error();
        Log::error(
            "BulkDownloadOperation: Download failed for: {}: {}", fullLocalPath(entry).string(), error.toString()
        );
        if (error.sftpError &&
            (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE ||
                error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED || error.sftpError->sftpError == SSH_FX_FAILURE))
        {
            // These errors are not critical for the overall bulk download, we can just skip the file and continue with
            // the next ones. Log and save the failed entry for reporting to the user later.
            failedEntries_.emplace_back(SharedData::fullPath(entries_, entry), error);
            completeCurrentDownload();

            if (options_.failFast)
            {
                Log::error("BulkDownloadOperation: failFast is enabled, failing the whole operation due to the error.");
                return enterErrorState<BulkDownloadOperation::WorkStatus>(error);
            }
            return WorkStatus::MoreWork;
        }
        return enterErrorState<BulkDownloadOperation::WorkStatus>(result.error());
    }
    else if (result.value() == WorkStatus::Complete)
    {
        // Download finished
        completeCurrentDownload();
    }
    return WorkStatus::MoreWork;
}

std::vector<std::pair<std::filesystem::path, BulkDownloadOperation::Error>> BulkDownloadOperation::getFailed() const
{
    return failedEntries_;
}

void BulkDownloadOperation::completeCurrentDownload()
{
    currentBytes_ += currentDownload_->totalSize();
    currentDownload_.reset();
    ++currentIndex_;
}

std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> BulkDownloadOperation::workAsArchive()
{
    // TODO: implement archive download
    return WorkStatus::Complete;
}

SharedData::OperationType BulkDownloadOperation::type() const
{
    return SharedData::OperationType::BulkDownload;
}

void BulkDownloadOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes)
{
    entries_ = std::move(entries);
    totalBytes_ = totalBytes;
}

std::expected<void, BulkDownloadOperation::Error> BulkDownloadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
        enterState(OperationState::Canceled);
    return {};
}

SecureShell::ProcessingStrand* BulkDownloadOperation::strand() const
{
    return sftp_->strand();
}