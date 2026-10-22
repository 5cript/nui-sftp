#include <backend/file_tracking/file_states.hpp>
#include <utility/error_message.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>

#include <chrono>
#include <system_error>

namespace FileTracking
{
    std::optional<FileState> fileStateOf(std::filesystem::directory_entry const& entry)
    {
        std::error_code entryError{};
        if (entry.is_symlink(entryError))
            return FileState{.path = entry.path(), .isSymlink = true};
        if (entry.is_directory(entryError))
            return FileState{.path = entry.path(), .isDirectory = true};
        if (!entry.is_regular_file(entryError))
            return std::nullopt;
        std::error_code sizeError{};
        const auto size = entry.file_size(sizeError);
        const auto modified = entry.last_write_time(entryError);
        if (sizeError || entryError)
            return std::nullopt;
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(modified).time_since_epoch());
        return FileState{
            .path = entry.path(),
            .size = static_cast<std::uint64_t>(size),
            .mtime = static_cast<std::uint64_t>(seconds.count()),
        };
    }

    std::vector<FileState> itemStatesOf(std::filesystem::path const& item)
    {
        std::vector<FileState> files;
        // Canonical like the watched directories, so the paths match the change events. Below a canonical path the
        // walk spells everything canonically.
        std::error_code canonicalError{};
        const auto canonicalItem = std::filesystem::weakly_canonical(item, canonicalError);
        std::error_code entryError{};
        const std::filesystem::directory_entry itemEntry{canonicalItem, entryError};
        if (auto state = fileStateOf(itemEntry))
            files.push_back(*std::move(state));
        std::error_code typeError{};
        if (!itemEntry.is_directory(typeError) || itemEntry.is_symlink(typeError))
            return files;

        std::error_code walkError{};
        for (std::filesystem::recursive_directory_iterator iter{canonicalItem, walkError}, end;
             !walkError && iter != end;
             iter.increment(walkError))
        {
            if (auto state = fileStateOf(*iter))
                files.push_back(*std::move(state));
        }
        if (walkError)
            Log::warn(
                "FileTracking: walking '{}' failed: {}", Paths::nativeToUtf8(item), Utility::errorMessage(walkError)
            );
        return files;
    }

    std::optional<SourceState> sourceStateOf(std::filesystem::path const& path)
    {
        std::error_code statusError{};
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, statusError)))
            return std::nullopt;
        std::error_code sizeError{};
        const auto size = std::filesystem::file_size(path, sizeError);
        std::error_code timeError{};
        const auto modified = std::filesystem::last_write_time(path, timeError);
        if (sizeError || timeError)
            return std::nullopt;
        return SourceState{.size = static_cast<std::uint64_t>(size), .modified = modified};
    }
}
