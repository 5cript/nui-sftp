#include <backend/sftp/local_scan_operation.hpp>
#include <log/log.hpp>

#include <ssh/sftp_session.hpp>

#include <filesystem>

LocalScanOperation::LocalScanOperation(ScanOperationOptions options)
    : localPath_{std::move(options.localPath)}
    , progressCallback_{std::move(options.progressCallback)}
{}

LocalScanOperation::~LocalScanOperation() = default;

std::expected<std::vector<SharedData::DirectoryEntry>, LocalScanOperation::Error>
LocalScanOperation::scanner(std::filesystem::path const& path)
{
    auto iter = std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied);
    const auto end = std::filesystem::directory_iterator();

    std::vector<SharedData::DirectoryEntry> entries;

    for (; iter != end; ++iter)
    {
        try
        {
            auto const& entry = *iter;
            SharedData::DirectoryEntry::FileType type;
            if (entry.is_directory())
                type = SharedData::DirectoryEntry::FileType::Directory;
            else if (entry.is_regular_file())
                type = SharedData::DirectoryEntry::FileType::Regular;
            else if (entry.is_symlink())
                type = SharedData::DirectoryEntry::FileType::Symlink;
            else
            {
                // Dont add anything thats not uploadable:
                continue;
            }

            entries.push_back(
                SharedData::DirectoryEntry{
                    .path = entry.path().filename(),
                    .type = type,
                    .size = entry.is_regular_file() ? entry.file_size() : 0
                }
            );
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Log::error("LocalScanOperation: Filesystem error while scanning '{}': {}", path.generic_string(), e.what());
            return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
                {.type = ErrorType::FilesystemError, .extraInfo = e.what()}
            );
        }
    }
    return {std::move(entries)};
}

std::uint64_t LocalScanOperation::totalBytes() const
{
    return walker_->totalBytes();
}

std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error> LocalScanOperation::work()
{
    using enum OperationState;

    switch (state_)
    {
        case (NotStarted):
        {
            state_ = Running;
            Log::info("LocalScanOperation: Starting scan of '{}'.", localPath_.generic_string());
            progressCallback_(0, 0, 0);
            return WorkStatus::MoreWork;
        }
        case (Running):
        {
            return withWalkerDo(
                [this](auto& walker) -> std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error>
                {
                    if (walker.completed())
                    {
                        Log::info("LocalScanOperation: Scan of '{}' completed.", localPath_.generic_string());
                        state_ = Completed;
                        return WorkStatus::Complete;
                    }

                    auto result = walker.walk();
                    if (!result.has_value())
                    {
                        Log::error("LocalScanOperation: Failed to scan directory: {}", result.error().toString());
                        return enterErrorState<WorkStatus>(result.error());
                    }
                    // -1, because the walker includes the base/root dir of the search:
                    progressCallback_(walker.totalBytes(), walker.currentIndex(), walker.totalEntries() - 1);
                    return WorkStatus::MoreWork;
                }
            );
        }
        case (Prepared):
        case (Preparing):
        case (Finalizing):
            Log::error("LocalScanOperation: Invalid state: {}", static_cast<int>(state_));
            return enterErrorState<WorkStatus>({.type = ErrorType::InvalidOperationState});
        case (Completed):
        {
            Log::warn("LocalScanOperation: Operation already completed.");
            // Dont enter error state here, it would overwrite the success state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
        }
        case (Failed):
        {
            Log::warn("LocalScanOperation: Operation already failed.");
            // Do not enter error state here, it would overwrite the error state.
            return std::unexpected(Error{.type = ErrorType::CannotWorkFailedOperation});
        }
        case (Canceled):
        {
            Log::warn("LocalScanOperation: Cannot work on canceled operation.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCanceledOperation});
        }
    }
    return enterErrorState<WorkStatus>({.type = ErrorType::UnknownWorkState});
}

std::expected<void, LocalScanOperation::Error> LocalScanOperation::cancel(bool adoptCancelState)
{
    if (adoptCancelState)
    {
        Log::info("LocalScanOperation: Scan of '{}' canceled.", localPath_.generic_string());
        enterState(OperationState::Canceled);
    }
    return {};
}