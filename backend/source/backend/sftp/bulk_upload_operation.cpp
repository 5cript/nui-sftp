#include <backend/sftp/bulk_upload_operation.hpp>
#include <ssh/sftp_session.hpp>
#include <log/log.hpp>

BulkUploadOperation::BulkUploadOperation(SecureShell::SftpSession& sftp, BulkUploadOperationOptions options)
    : Operation{}
    , sftp_{&sftp}
    , options_{std::move(options)}
    , currentUpload_{nullptr}
    , entries_{}
    , totalBytes_{0}
    , currentIndex_{0}
    , currentBytes_{0}
    , futureTimeout_{options_.individualOptions.futureTimeout}
{}

BulkUploadOperation::~BulkUploadOperation() = default;

std::expected<BulkUploadOperation::WorkStatus, BulkUploadOperation::Error> BulkUploadOperation::work()
{
    return workNormal();
}

std::filesystem::path BulkUploadOperation::fullLocalPath(SharedData::DirectoryEntry const& entry) const
{
    return (options_.localPath / SharedData::fullPathRelative(entries_, entry)).lexically_normal();
}

std::filesystem::path BulkUploadOperation::fullRemotePath(SharedData::DirectoryEntry const& entry) const
{
    return (options_.remotePath / SharedData::fullPathRelative(entries_, entry)).lexically_normal();
}

std::expected<BulkUploadOperation::WorkStatus, BulkUploadOperation::Error> BulkUploadOperation::workNormal()
{
    using enum OperationState;

    switch (state())
    {
        case (NotStarted):
        {
            if (entries_.empty())
            {
                Log::info("BulkUploadOperation: No entries to upload.");
                enterState(Completed);
                return WorkStatus::Complete;
            }
            auto const& entry = entries_[0];
            if (entry.isDirectory())
            {
                // Base directory of the upload, everything after this will be relative to this:
                auto result = createDirectory(options_.remotePath, entry);
                if (!result.has_value())
                    return enterErrorState<BulkUploadOperation::WorkStatus>(result.error());
            }
            else
            {
                Log::error("BulkUploadOperation: First entry is not a directory: {}.", fullLocalPath(entry).string());
                return enterErrorState<BulkUploadOperation::WorkStatus>(
                    Error{.type = ErrorType::ImplementationError, .extraInfo = "First entry must be a directory."}
                );
            }
            currentIndex_ = 1;
            enterState(Running);
            options_.overallProgressCallback(
                options_.remotePath, currentIndex_, entries_.size() - 1, 0, 0, 0, totalBytes_, 0
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
                Log::info("BulkUploadOperation: Bulk upload completed.");
                enterState(Completed);
                return WorkStatus::Complete;
            }
            if (currentIndex_ > entries_.size())
            {
                Log::error("BulkUploadOperation: Current index out of range.");
                return enterErrorState<BulkUploadOperation::WorkStatus>(Error{
                    .type = ErrorType::ImplementationError,
                    .extraInfo = "Bulk upload index is beyond the item count, which should never occur."
                });
            }

            auto const& entry = entries_[currentIndex_];
            if (entry.isDirectory())
            {
                // Create directory:
                const auto result = createDirectory(fullRemotePath(entry), entry);
                if (!result.has_value())
                    return enterErrorState<BulkUploadOperation::WorkStatus>(result.error());

                ++currentIndex_;
                options_.overallProgressCallback(
                    fullRemotePath(entry), currentIndex_, entries_.size() - 1, 0, 0, currentBytes_, totalBytes_, 0
                );
            }
            else if (entry.isRegularFile())
            {
                if (!currentUpload_)
                {
                    auto individualOpts = options_.individualOptions;
                    individualOpts.remotePath = fullRemotePath(entry);
                    individualOpts.localPath = fullLocalPath(entry);
                    individualOpts.progressCallback = [this](auto, auto max, auto current, auto bytesPerSecond)
                    {
                        options_.overallProgressCallback(
                            fullRemotePath(entries_[currentIndex_]),
                            currentIndex_,
                            entries_.size() - 1,
                            current,
                            max,
                            currentBytes_ + current,
                            totalBytes_,
                            bytesPerSecond
                        );
                    };
                    currentUpload_ = std::make_unique<UploadOperation>(*sftp_, individualOpts);
                }
                else
                {
                    return workCurrentFile();
                }
            }
            else if (entry.isSymlink())
            {
                // TODO: handle symlink
                Log::warn(
                    "BulkUploadOperation: Symlinks are not yet supported for entry: {}.", fullRemotePath(entry).string()
                );

                // Under linux:
                // - symlinks that are within the downloaded structure shall be downloaded
                //   as symlinks.
                // - symlinks that are outside the downloaded structure shall be downloaded
                //   as regular files? or not? or also as symlinks? => probably also as links
                // Under windows we should ask the user earlier how they want to proceed with them (copies? ignore?
                // windows links - yuck?).

                ++currentIndex_;
            }
            else
            {
                Log::warn(
                    "BulkUploadOperation: Skipping unsupported file type for entry: {}.", fullRemotePath(entry).string()
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
    }
}

std::expected<void, BulkUploadOperation::Error>
BulkUploadOperation::createDirectory(std::filesystem::path const& path, SharedData::DirectoryEntry const& entry)
{
    auto fut = sftp_->createDirectory(path, determinePerms(entry));
    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
    {
        Log::error("BulkUploadOperation: Failed to create remote sftp directory: timeout.");
        return std::unexpected(
            Error{
                .type = ErrorType::FutureTimeout,
                .extraInfo = fmt::format("Timeout creating remote directory: {}", path.string())
            }
        );
    }
    auto result = fut.get();
    if (!result.has_value())
    {
        Log::error("BulkUploadOperation: Failed to create remote sftp directory: {}.", result.error().message);
        return std::unexpected(
            Error{
                .type = ErrorType::SftpError,
                .sftpError = result.error(),
                .extraInfo = fmt::format("Creating remote directory: {}", path.string())
            }
        );
    }
    return {};
}

std::filesystem::perms BulkUploadOperation::determinePerms(SharedData::DirectoryEntry const& entry) const
{
    if (options_.individualOptions.inheritPermissions)
    {
        auto inherited = entry.permissions;
        if (entry.isRegularFile())
            inherited |= std::filesystem::perms::owner_write;
        return inherited;
    }
    else
    {
        if (entry.isDirectory())
        {
            if (options_.individualOptions.directoryPermissions)
                return options_.individualOptions.directoryPermissions.value() | std::filesystem::perms::owner_write;
            return (
                std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec
            );
        }
        else
        {
            if (options_.individualOptions.filePermissions)
                return options_.individualOptions.filePermissions.value() | std::filesystem::perms::owner_write;
            // inherit execute permissions anyway:
            const auto ownerExecute = entry.permissions & std::filesystem::perms::owner_exec;
            const auto groupExecute = entry.permissions & std::filesystem::perms::group_exec;
            return (
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | ownerExecute |
                std::filesystem::perms::group_read | std::filesystem::perms::group_write | groupExecute
            );
        }
    }
}

std::expected<BulkUploadOperation::WorkStatus, BulkUploadOperation::Error> BulkUploadOperation::workCurrentFile()
{
    auto result = currentUpload_->work();
    if (!result)
    {
        auto const& entry = entries_[currentIndex_];
        Log::error(
            "BulkUploadOperation: Upload failed for file: {}: {}",
            fullRemotePath(entry).string(),
            result.error().toString()
        );
        return enterErrorState<BulkUploadOperation::WorkStatus>(result.error());
    }
    else if (result.value() == WorkStatus::Complete)
    {
        // Upload finished
        currentBytes_ += currentUpload_->totalSize();
        currentUpload_.reset();
        ++currentIndex_;
    }
    return WorkStatus::MoreWork;
}

SharedData::OperationType BulkUploadOperation::type() const
{
    return SharedData::OperationType::BulkUpload;
}

void BulkUploadOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes)
{
    entries_ = std::move(entries);
    totalBytes_ = totalBytes;
}

std::expected<void, BulkUploadOperation::Error> BulkUploadOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
        enterState(OperationState::Canceled);
    return {};
}

SecureShell::ProcessingStrand* BulkUploadOperation::strand() const
{
    return sftp_->strand();
}