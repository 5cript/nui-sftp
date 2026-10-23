#pragma once

#include <backend/file_tracking/tracking_state.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace FileTracking
{
    /**
     * @brief What a watched path holds locally. Uploads and downloads carry the modification time along, so after one
     *        it is what the server holds too.
     */
    struct FileState
    {
        std::filesystem::path path;
        bool isDirectory{false};
        /** @brief A symbolic link, not followed: its state is the server's own, see TrackerRemote::linkStat. */
        bool isSymlink{false};
        /** @brief Bytes, zero for a directory and a symbolic link. */
        std::uint64_t size{0};
        /** @brief Seconds, zero for a directory and a symbolic link. */
        std::uint64_t mtime{0};

        friend bool operator==(FileState const&, FileState const&) = default;
    };

    /**
     * @brief The state of @p entry, the path spelled as the entry spells it, canonical when the walk that found it
     *        started at a canonical path. Nothing for anything but a regular file, a directory or a symbolic link and
     *        for an entry that cannot be read.
     */
    std::optional<FileState> fileStateOf(std::filesystem::directory_entry const& entry);

    /**
     * @brief The states (see fileStateOf) of @p item and of everything below it when it is a directory, the paths
     *        canonical. Symbolic links below it are not followed.
     */
    std::vector<FileState> itemStatesOf(std::filesystem::path const& item);

    /**
     * @brief How many entries @p item is with everything below it when it is a directory, symbolic links not
     *        followed. Their states are not read.
     */
    std::size_t entryCountOf(std::filesystem::path const& item);

    /**
     * @brief The state of the regular file at @p path, nothing for anything else, a symbolic link too, and when it
     *        cannot be read.
     */
    std::optional<SourceState> sourceStateOf(std::filesystem::path const& path);
}
