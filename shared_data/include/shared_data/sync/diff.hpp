#pragma once

#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SharedData::Sync
{
    enum class Direction : std::uint8_t
    {
        Both,
        Upload,
        Download
    };

    enum class Action : std::uint8_t
    {
        Upload,
        Download,
        DeleteLocal,
        DeleteRemote
    };

    /**
     * @brief Inputs that govern which differences become actionable items.
     */
    struct DiffOptions
    {
        Direction direction{Direction::Both};
        bool actionUpload{true};
        bool actionDownload{true};
        bool actionDelete{false};
        /// When false, entries below the root directory are dropped from the diff.
        bool recursive{true};
        /// When true, entries with any path segment starting with '.' are dropped.
        bool ignoreHidden{false};
    };

    /**
     * @brief One actionable difference between the local and remote scan results.
     *
     * The relKey is a posix-style path relative to the corresponding scan root and
     * doubles as a stable identifier across recompares.
     */
    struct DiffEntry
    {
        std::string relKey;
        Action action;
        std::optional<DirectoryEntry> local;
        std::optional<DirectoryEntry> remote;
    };

    struct DiffResult
    {
        std::vector<DiffEntry> uploads;
        std::vector<DiffEntry> downloads;
        std::vector<DiffEntry> deletes;
    };

    /**
     * @brief Returns true when two entries should be treated as differing.
     *
     * Symlinks compare by raw link target (size/mtime of the link itself are
     * meaningless). Type mismatches always count as a difference.
     */
    bool entriesDiffer(DirectoryEntry const& localEntry, DirectoryEntry const& remoteEntry);

    /**
     * @brief Computes the upload / download / delete diff lists from two scan results.
     */
    DiffResult computeSyncDiff(
        std::filesystem::path const& localRoot,
        std::filesystem::path const& remoteRoot,
        std::vector<DirectoryEntry> const& localEntries,
        std::vector<DirectoryEntry> const& remoteEntries,
        DiffOptions const& options
    );
}
