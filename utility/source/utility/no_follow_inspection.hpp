#pragma once

#include <utility/link_safe_remove.hpp>

#include <filesystem>
#include <system_error>

namespace Utility::Detail
{
    /**
     * @brief What an item is, inspected without following it, with what removing it needs to know.
     */
    struct NoFollowInspection
    {
        NoFollowKind kind{NoFollowKind::Missing};
        /** @brief The item has the directory attribute, which on Windows includes directory links and junctions. */
        bool directoryEntry{false};
    };

    /**
     * @brief Inspects @p path without following it, a missing path is no error.
     */
    NoFollowInspection inspectNoFollow(std::filesystem::path const& path, std::error_code& error);

#ifndef _WIN32
    /**
     * @brief The inspection a symlink_status gives, which on POSIX is all there is to know.
     */
    NoFollowInspection inspectionOf(std::filesystem::file_status const& status);
#endif

    /**
     * @brief Removes the item at @p path that @p inspection describes, never following it.
     * @return True when it was removed, false when it was gone or @p error is set.
     */
    bool removeInspected(std::filesystem::path const& path, NoFollowInspection const& inspection, std::error_code& error);
}
