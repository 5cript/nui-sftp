#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/local_file_problem.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/permissions.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>

#include <ssh/sftp_session.hpp>

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
    , recursive_{options.recursive}
    , ignoreHidden_{options.ignoreHidden}
    , buildTree_{options.buildTree}
    , excludedSuffixes_{std::move(options.excludedSuffixes)}
    , failureToast_{options.failureToast}
{}

LocalScanOperation::~LocalScanOperation() = default;

std::expected<std::vector<SharedData::DirectoryEntry>, LocalScanOperation::Error>
LocalScanOperation::scanner(Paths::RelativePath const& relative)
{
    // Non-recursive mode: only list the root directory.  The walker still invokes the
    // scanner for each child directory it discovers, but we short-circuit those calls so
    // nothing below the root is enumerated.
    if (!recursive_ && rootScanned_)
        return std::vector<SharedData::DirectoryEntry>{};
    rootScanned_ = true;

    const auto joined = localPath_.join(relative);
    if (!joined)
    {
        return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
            {.type = ErrorType::InvalidName, .extraInfo = joined.error().toString()}
        );
    }
    const auto path = joined->native();

    // No skip_permission_denied: an unreadable directory would list as empty and sync would delete the
    // remote copy of its content.
    std::error_code iterErrc{};
    auto iter = std::filesystem::directory_iterator(path, iterErrc);
    if (iterErrc)
        return listingFailed(path, relative, iterErrc);
    const auto end = std::filesystem::directory_iterator();

    if (respectIgnoreFiles_)
    {
        for (auto const& ignoreName : {".gitignore", ".ignore"})
        {
            const auto ignorePath = path / ignoreName;
            std::error_code errc{};
            if (!std::filesystem::is_regular_file(ignorePath, errc))
                continue;
            if (auto content = readLocalFile(ignorePath))
                ignoreMatcher_.addFile(relative, *content);
        }
    }

    std::vector<SharedData::DirectoryEntry> entries;

    for (; iter != end; iter.increment(iterErrc))
    {
        if (iterErrc)
            return listingFailed(path, relative, iterErrc);
        auto const& entry = *iter;
        const auto failed = [this, &entry](std::error_code const& error)
        {
            Log::error(
                failureToast_,
                "LocalScanOperation: Cannot inspect '{}': {}",
                Paths::nativeToUtf8(entry.path()),
                Utility::errorMessage(error)
            );
            return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
                {
                    .type = ErrorType::FilesystemError,
                    .extraInfo = Utility::errorMessage(error),
                    .localProblem = localFileProblemOf(error),
                }
            );
        };
        std::error_code statusError{};
        const auto status = entry.symlink_status(statusError);
        if (statusError)
            return failed(statusError);
        SharedData::DirectoryEntry::FileType type;
#ifdef _WIN32
        // A junction is a reparse point that std::filesystem reports as a directory.
        std::error_code linkError{};
        const bool isSymlink =
            std::filesystem::is_symlink(status) || Utility::isLinkOrJunction(entry.path(), linkError);
#else
        const bool isSymlink = std::filesystem::is_symlink(status);
#endif
        if (isSymlink)
        {
#ifdef _WIN32
            // Windows symlinks and junctions are not transferred. A sync tree keeps them as excluded
            // placeholders, so the name still counts as present and the remote side is left alone.
            if (!buildTree_)
            {
                skippedLinks_.push_back(Paths::nativeToUtf8(entry.path()));
                continue;
            }
            type = SharedData::DirectoryEntry::FileType::Unknown;
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
            // Sockets, devices and FIFOs are not uploadable, a sync tree keeps them as excluded placeholders.
            if (!buildTree_)
                continue;
            type = SharedData::fileTypeFromStdFilesystemType(status.type());
            if (type == SharedData::DirectoryEntry::FileType::Regular ||
                type == SharedData::DirectoryEntry::FileType::Directory)
                type = SharedData::DirectoryEntry::FileType::Unknown;
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
                mtimeSecs = ComparedStateCheck::secondsSinceEpoch(ftime);
        }

        std::optional<std::string> linkTarget{};
        if (isSymlink)
        {
            std::error_code errc{};
            const auto target = std::filesystem::read_symlink(entry.path(), errc);
            if (!errc)
                linkTarget = Paths::nativeToUtf8(target);
        }

        auto converted = Paths::convertNativeToUtf8(entry.path().filename());
        auto const& name = converted.utf8;
        if (ignoreHidden_ && !name.empty() && name.front() == '.')
            continue;
        if (respectIgnoreFiles_ && !ignoreMatcher_.empty())
        {
            // A name that is no segment cannot match a rule, the walker rejects it.
            const auto segment = Paths::PathSegment::make(name);
            if (segment &&
                ignoreMatcher_.isIgnored(relative / *segment, type == SharedData::DirectoryEntry::FileType::Directory))
                continue;
        }

        // The size of what a followed link points to, like a regular file's own.
        std::error_code sizeError{};
        const auto size = entry.is_regular_file(sizeError) ? entry.file_size(sizeError) : 0u;
        if (sizeError && !isSymlink)
            return failed(sizeError);

        entries.push_back(
            SharedData::DirectoryEntry{
                .name = std::move(converted.utf8),
                .nameIsValidUtf8 = converted.lossless,
                .type = type,
                .size = sizeError ? 0u : size,
                .permissions = Utility::localPermissions(status),
                .mtime = mtimeSecs,
                .linkTarget = std::move(linkTarget),
            }
        );
    }
    return {std::move(entries)};
}

std::expected<std::vector<SharedData::DirectoryEntry>, LocalScanOperation::Error>
LocalScanOperation::listingFailed(
    std::filesystem::path const& path,
    Paths::RelativePath const& relative,
    std::error_code const& errorCode
)
{
    const bool belowRoot = !relative.empty();
    const bool deniedOrVanished =
        errorCode == std::errc::permission_denied || errorCode == std::errc::no_such_file_or_directory;
    if (belowRoot && deniedOrVanished)
    {
        Log::warn(
            "LocalScanOperation: Cannot list '{}': {}", Paths::nativeToUtf8(path), Utility::errorMessage(errorCode)
        );
        if (!buildTree_)
        {
            // The consumer reports it instead of transferring an empty directory.
            unreadableDirectories_.push_back(Paths::nativeToUtf8(path));
            unreadableRelatives_.insert(relative.value());
            return std::vector<SharedData::DirectoryEntry>{};
        }
        return std::unexpected(
            Error{
                .type = ErrorType::DirectoryUnreadable,
                .extraInfo = fmt::format("{}: {}", Paths::nativeToUtf8(path), Utility::errorMessage(errorCode)),
            }
        );
    }
    Log::error(
        failureToast_,
        "LocalScanOperation: Failed to list '{}': {}",
        Paths::nativeToUtf8(path),
        Utility::errorMessage(errorCode)
    );
    return enterErrorState<std::vector<SharedData::DirectoryEntry>>(
        {
            .type = ErrorType::FilesystemError,
            .extraInfo = Utility::errorMessage(errorCode),
            .localProblem = localFileProblemOf(errorCode),
        }
    );
}

void LocalScanOperation::markUnreadable(std::vector<SharedData::DirectoryEntry>& entries) const
{
    if (unreadableRelatives_.empty())
        return;
    for (auto& entry : entries)
    {
        if (!entry.isDirectory())
            continue;
        if (const auto relative = SharedData::relativePath(entries, entry);
            relative && unreadableRelatives_.contains(relative->value()))
            entry.type = SharedData::FileType::Unknown;
    }
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
            if (localPath_.empty())
            {
                Log::error(failureToast_, "LocalScanOperation: No root to scan.");
                return enterErrorState<WorkStatus>(
                    {.type = ErrorType::ImplementationError, .extraInfo = "The scan has no root."}
                );
            }
            state_ = Running;
            Log::info("LocalScanOperation: Starting scan of '{}'.", localPath_.toUtf8());
            progressCallback_(0, 0, 0);
            return WorkStatus::MoreWork;
        }
        case (Running):
        {
            auto stepWalker =
                [this](auto& walker) -> std::expected<LocalScanOperation::WorkStatus, LocalScanOperation::Error>
            {
                if (walker.completed())
                {
                    Log::info("LocalScanOperation: Scan of '{}' completed.", localPath_.toUtf8());
                    state_ = Completed;
                    return WorkStatus::Complete;
                }

                auto result = walker.walk();
                if (!result.has_value())
                {
                    Log::error(
                        failureToast_, "LocalScanOperation: Failed to scan directory: {}", result.error().toString()
                    );
                    return enterErrorState<WorkStatus>(result.error());
                }
                // -1, because the walker includes the base/root dir of the search:
                progressCallback_(walker.totalBytes(), walker.currentIndex(), walker.totalEntries() - 1);
                return WorkStatus::MoreWork;
            };
            if (buildTree_)
                return withTreeWalkerDo(stepWalker);
            return withWalkerDo(stepWalker);
        }
        case (Prepared):
        case (Preparing):
        case (Finalizing):
            Log::error(failureToast_, "LocalScanOperation: Invalid state: {}", static_cast<int>(state_));
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
        Log::info("LocalScanOperation: Scan of '{}' canceled.", localPath_.toUtf8());
        enterState(OperationState::Canceled);
    }
    return {};
}