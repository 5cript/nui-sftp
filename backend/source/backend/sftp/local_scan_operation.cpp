#include <backend/sftp/local_scan_operation.hpp>
#include <log/log.hpp>

#include <ssh/sftp_session.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#    include <sys/stat.h>
#endif

namespace
{
    std::optional<std::string> readLocalFile(std::filesystem::path const& path)
    {
        std::ifstream stream{path, std::ios::binary};
        if (!stream)
            return std::nullopt;
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }
}

LocalScanOperation::LocalScanOperation(ScanOperationOptions options)
    : localPath_{std::move(options.localPath)}
    , progressCallback_{std::move(options.progressCallback)}
    , respectIgnoreFiles_{options.respectIgnoreFiles}
{}

LocalScanOperation::~LocalScanOperation() = default;

std::expected<std::vector<SharedData::DirectoryEntry>, LocalScanOperation::Error>
LocalScanOperation::scanner(std::filesystem::path const& path)
{
    auto iter = std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied);
    const auto end = std::filesystem::directory_iterator();

    const auto relDir = [&]() -> std::filesystem::path {
        std::error_code errc{};
        auto rel = std::filesystem::relative(path, localPath_, errc);
        if (errc || rel.empty() || rel == std::filesystem::path{"."})
            return {};
        return rel;
    }();

    if (respectIgnoreFiles_)
    {
        for (auto const& ignoreName : {".gitignore", ".ignore"})
        {
            const auto ignorePath = path / ignoreName;
            std::error_code errc{};
            if (!std::filesystem::is_regular_file(ignorePath, errc))
                continue;
            if (auto content = readLocalFile(ignorePath))
                ignoreMatcher_.addFile(relDir, *content);
        }
    }

    std::vector<SharedData::DirectoryEntry> entries;

    for (; iter != end; ++iter)
    {
        try
        {
            auto const& entry = *iter;
            const auto status = entry.symlink_status();
            SharedData::DirectoryEntry::FileType type;
            const bool isSymlink = std::filesystem::is_symlink(status);
            if (isSymlink)
            {
#ifdef _WIN32
                // Windows symlinks require elevated privileges to create and are rarely used by
                // ordinary users. Skipping them here keeps sync predictable on Windows.
                continue;
#else
                type = SharedData::DirectoryEntry::FileType::Symlink;
#endif
            }
            else if (std::filesystem::is_directory(status))
                type = SharedData::DirectoryEntry::FileType::Directory;
            else if (std::filesystem::is_regular_file(status))
                type = SharedData::DirectoryEntry::FileType::Regular;
            else
            {
                // Dont add anything thats not uploadable:
                continue;
            }

            std::uint64_t mtimeSecs = 0;
            if (isSymlink)
            {
#ifndef _WIN32
                // For symlinks we want the link's own mtime (lstat), not the target's.
                struct ::stat st{};
                if (::lstat(entry.path().c_str(), &st) == 0)
                    mtimeSecs = static_cast<std::uint64_t>(st.st_mtime);
#endif
            }
            else
            {
                std::error_code errc{};
                const auto ftime = entry.last_write_time(errc);
                if (!errc)
                {
                    const auto sctime = std::chrono::file_clock::to_sys(ftime);
                    mtimeSecs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(sctime.time_since_epoch()).count()
                    );
                }
            }

            std::optional<std::filesystem::path> linkTarget{};
            if (isSymlink)
            {
                std::error_code errc{};
                auto target = std::filesystem::read_symlink(entry.path(), errc);
                if (!errc)
                    linkTarget = std::move(target);
            }

            const auto filename = entry.path().filename();
            if (respectIgnoreFiles_ && !ignoreMatcher_.empty())
            {
                const auto childRel = relDir.empty() ? filename : (relDir / filename);
                if (ignoreMatcher_.isIgnored(childRel, type == SharedData::DirectoryEntry::FileType::Directory))
                    continue;
            }

            entries.push_back(
                SharedData::DirectoryEntry{
                    .path = filename,
                    .type = type,
                    .size = entry.is_regular_file() ? entry.file_size() : 0,
                    .mtime = mtimeSecs,
                    .linkTarget = std::move(linkTarget),
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
        case (PartialSuccess):
        {
            Log::warn("LocalScanOperation: Operation completed with partial success.");
            return std::unexpected(Error{.type = ErrorType::CannotWorkCompletedOperation});
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