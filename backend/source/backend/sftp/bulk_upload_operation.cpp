#include <backend/sftp/bulk_upload_operation.hpp>
#include <ssh/sftp_session.hpp>
#include <constants/sftp.hpp>
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
    if (!prescannedPathOverride_.empty())
    {
        for (std::size_t idx = 0; idx < entries_.size(); ++idx)
        {
            if (&entries_[idx] == &entry)
                return prescannedPathOverride_[idx].first;
        }
    }
    return (options_.localPath / SharedData::fullPathRelative(entries_, entry)).lexically_normal();
}

std::filesystem::path BulkUploadOperation::fullRemotePath(SharedData::DirectoryEntry const& entry) const
{
    if (!prescannedPathOverride_.empty())
    {
        for (std::size_t idx = 0; idx < entries_.size(); ++idx)
        {
            if (&entries_[idx] == &entry)
                return prescannedPathOverride_[idx].second;
        }
    }
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
            if (!prescannedPathOverride_.empty())
            {
                // Prescanned flat-file mode — see BulkDownloadOperation for
                // the rationale.  Each file's UploadOperation creates its
                // missing remote parents via createMissingDirectories.
                currentIndex_ = 0;
                enterState(Running);
                options_.overallProgressCallback(
                    options_.remotePath, currentIndex_, entries_.size(), 0, 0, 0, totalBytes_, bulkBytesPerSecond_
                );
                return WorkStatus::MoreWork;
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
                options_.remotePath, currentIndex_, entries_.size() - 1, 0, 0, 0, totalBytes_, bulkBytesPerSecond_
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
                // In prescanned mode, emit a terminal progress event so the
                // frontend sees "N/N" (and the per-row index-based mark-green
                // pass in sync_dialog sweeps the final file).  Tree-scan mode
                // already lands at "(N-1)/(N-1)" on the last file's progress
                // tick because entries_[0] is the synthetic root — emitting
                // another event here would regress it to "N/(N-1)".
                if (!prescannedPathOverride_.empty())
                {
                    options_.overallProgressCallback(
                        options_.remotePath,
                        currentIndex_,
                        entries_.size(),
                        0,
                        0,
                        currentBytes_,
                        totalBytes_,
                        bulkBytesPerSecond_
                    );
                }
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
                // Tree-scan mode treats entries_[0] as the synthetic root dir
                // (not counted toward the user-visible total); prescanned mode
                // has no synthetic root, so every entry counts.
                const std::uint64_t fileCount = entries_.size() - (prescannedPathOverride_.empty() ? 1 : 0);
                options_.overallProgressCallback(
                    fullRemotePath(entry),
                    currentIndex_,
                    fileCount,
                    0,
                    0,
                    currentBytes_,
                    totalBytes_,
                    bulkBytesPerSecond_
                );
            }
            else if (entry.isRegularFile() || entry.isSymlink())
            {
                if (!currentUpload_)
                {
                    auto individualOpts = options_.individualOptions;
                    individualOpts.remotePath = fullRemotePath(entry);
                    individualOpts.localPath = fullLocalPath(entry);
                    individualOpts.bigFileOptimized = entry.size >= Constants::bigFileCutOff;
                    individualOpts.progressCallback = [this](auto, auto max, auto current, auto /*bytesPerSecond*/)
                    {
                        const std::uint64_t fileCount = entries_.size() - (prescannedPathOverride_.empty() ? 1 : 0);
                        const auto bytesNow = currentBytes_ + current;
                        options_.overallProgressCallback(
                            fullRemotePath(entries_[currentIndex_]),
                            currentIndex_,
                            fileCount,
                            current,
                            max,
                            bytesNow,
                            totalBytes_,
                            updateBulkBytesPerSecond(bytesNow)
                        );
                    };
                    individualOpts.symlinkHandling = options_.individualOptions.symlinkHandling;
                    currentUpload_ = std::make_unique<UploadOperation>(*sftp_, individualOpts);
                }
                else
                {
                    return workCurrentFile();
                }
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
        case (PartialSuccess):
        {
            Log::warn("DownloadOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
    }
}

std::make_signed_t<std::size_t> BulkUploadOperation::updateBulkBytesPerSecond(std::uint64_t bytesNow)
{
    // Cadence/smoothing match BulkDownloadOperation — see that implementation
    // for the rationale.
    using namespace std::chrono_literals;
    constexpr auto sampleInterval = 1000ms;
    constexpr double smoothingAlpha = 0.3;

    const auto now = std::chrono::steady_clock::now();
    if (lastBpsSampleTime_.time_since_epoch().count() == 0)
    {
        lastBpsSampleTime_ = now;
        lastBpsSampleBytes_ = bytesNow;
        return bulkBytesPerSecond_;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBpsSampleTime_);
    if (duration < sampleInterval)
        return bulkBytesPerSecond_;
    const std::uint64_t bytesDelta = bytesNow >= lastBpsSampleBytes_ ? (bytesNow - lastBpsSampleBytes_) : 0;
    const double rawBps = static_cast<double>(bytesDelta * 1000) / static_cast<double>(duration.count());
    const double smoothed = bulkBytesPerSecond_ == 0
        ? rawBps
        : smoothingAlpha * rawBps + (1.0 - smoothingAlpha) * static_cast<double>(bulkBytesPerSecond_);
    bulkBytesPerSecond_ = static_cast<std::make_signed_t<std::size_t>>(smoothed);
    lastBpsSampleTime_ = now;
    lastBpsSampleBytes_ = bytesNow;
    return bulkBytesPerSecond_;
}

std::expected<void, BulkUploadOperation::Error>
BulkUploadOperation::createDirectory(std::filesystem::path const& path, SharedData::DirectoryEntry const& entry)
{
    auto fut = sftp_->createDirectoryIfItDoesntExist(path, determinePerms(entry));
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
        const auto error = result.error();
        Log::error(
            "BulkUploadOperation: Upload failed for file: {}: {}", fullRemotePath(entry).string(), error.toString()
        );
        if (!error.sftpError ||
            (error.sftpError &&
                (error.sftpError->sftpError == SSH_FX_NO_SUCH_FILE ||
                    error.sftpError->sftpError == SSH_FX_PERMISSION_DENIED ||
                    error.sftpError->sftpError == SSH_FX_FAILURE)))
        {
            // These errors are not critical for the overall bulk upload, we can just skip the file and continue with
            // the next ones. Log and save the failed entry for reporting to the user later.
            failedEntries_.emplace_back(SharedData::fullPath(entries_, entry), error);
            completeCurrentUpload();

            if (options_.failFast)
            {
                Log::error("BulkUploadOperation: failFast is enabled, failing the whole operation due to the error.");
                return enterErrorState<BulkUploadOperation::WorkStatus>(error);
            }
            return WorkStatus::MoreWork;
        }
        return enterErrorState<BulkUploadOperation::WorkStatus>(result.error());
    }
    else if (result.value() == WorkStatus::Complete)
    {
        // Upload finished
        completeCurrentUpload();
    }
    return WorkStatus::MoreWork;
}

std::vector<std::pair<std::filesystem::path, BulkUploadOperation::Error>> BulkUploadOperation::getFailed() const
{
    return failedEntries_;
}

void BulkUploadOperation::completeCurrentUpload()
{
    currentBytes_ += currentUpload_->totalSize();
    currentUpload_.reset();
    ++currentIndex_;
    if (!prescannedPathOverride_.empty() && currentIndex_ == entries_.size())
    {
        options_.overallProgressCallback(
            options_.remotePath, currentIndex_, entries_.size(), 0, 0, currentBytes_, totalBytes_, bulkBytesPerSecond_
        );
    }
}

SharedData::OperationType BulkUploadOperation::type() const
{
    return SharedData::OperationType::BulkUpload;
}

void BulkUploadOperation::setScanResult(std::vector<SharedData::DirectoryEntry>&& entries, std::uint64_t totalBytes)
{
    entries_ = std::move(entries);
    totalBytes_ = totalBytes;
    prescannedPathOverride_.clear();
}

void BulkUploadOperation::setPrescannedFileList(std::vector<PrescannedFile> files)
{
    entries_.clear();
    entries_.reserve(files.size());
    prescannedPathOverride_.clear();
    prescannedPathOverride_.reserve(files.size());
    totalBytes_ = 0;
    for (auto& file : files)
    {
        SharedData::DirectoryEntry entry{};
        entry.path = file.localSrc;
        entry.fullPath = file.localSrc;
        entry.type = SharedData::FileType::Regular;
        entry.size = file.sizeBytes;
        totalBytes_ += file.sizeBytes;
        entries_.push_back(std::move(entry));
        prescannedPathOverride_.emplace_back(file.localSrc, file.remoteDst);
    }
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