#include <backend/sftp/bulk_download_operation.hpp>
#include <ssh/sftp_session.hpp>
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
            else if (entry.isRegularFile())
            {
                if (!currentDownload_)
                {
                    const auto remoteFullPath = SharedData::fullPath(entries_, entry);

                    auto fut = sftp_->openFile(
                        remoteFullPath, SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown
                    );

                    if (fut.wait_for(futureTimeout_) != std::future_status::ready)
                    {
                        Log::error("BulkDownloadOperation: Failed to open remote sftp file: timeout.");
                        return enterErrorState<BulkDownloadOperation::WorkStatus>(Error{
                            .type = ErrorType::FutureTimeout,
                            .extraInfo = fmt::format("Timeout opening remote file: {}", entry.path.string())
                        });
                    }

                    const auto openResult = fut.get();
                    if (!openResult.has_value())
                    {
                        Log::error(
                            "BulkDownloadOperation: Failed to open remote sftp file: {}.", openResult.error().message
                        );
                        return enterErrorState<BulkDownloadOperation::WorkStatus>(Error{
                            .type = ErrorType::SftpError,
                            .sftpError = openResult.error(),
                            .extraInfo = fmt::format("Opening remote file: {}", entry.path.string())
                        });
                    }

                    auto downloadOptions = options_.individualOptions;
                    downloadOptions.remotePath = remoteFullPath;
                    downloadOptions.localPath = fullLocalPath(entry);

                    downloadOptions.progressCallback = [this, operationId = this->id(), remoteFullPath](
                                                           auto min, auto max, auto current, auto bytesPerSecond
                                                       )
                    {
                        // Update current bytes
                        currentBytes_ += (current - min);

                        // Call overall progress callback
                        options_.overallProgressCallback(
                            remoteFullPath,
                            currentIndex_,
                            entries_.size() - 1,
                            current,
                            max,
                            currentBytes_,
                            totalBytes_,
                            bytesPerSecond
                        );
                    };

                    currentDownload_ =
                        std::make_unique<DownloadOperation>(std::move(openResult).value(), downloadOptions);
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
                    "BulkDownloadOperation: Symlinks are not yet supported for entry: {}.",
                    fullLocalPath(entry).string()
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
        Log::error(
            "BulkDownloadOperation: Download failed for file: {}: {}",
            fullLocalPath(entry).string(),
            result.error().toString()
        );
        return enterErrorState<BulkDownloadOperation::WorkStatus>(result.error());
    }
    else if (result.value() == WorkStatus::Complete)
    {
        // Download finished
        currentDownload_.reset();
        ++currentIndex_;
    }
    return WorkStatus::MoreWork;
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