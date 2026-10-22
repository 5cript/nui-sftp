#pragma once

#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace SharedData
{
    /**
     * @brief What a sync compare saw at a path that one of its entries replaces or deletes: nothing, or the type,
     *        size and modification time of what was there, the link literal for a symlink.
     */
    struct ComparedState
    {
        bool exists{false};
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint64_t mtime{0};
        std::optional<std::string> linkTarget{};
        /**
         * @brief Something seen present must still be there, for a file tracker that asks before recreating what the
         *        server lost. A sync leaves it off.
         */
        bool mustStillExist{false};

        bool operator==(ComparedState const&) const = default;
    };

    /**
     * @brief @p entry, as an lstat or a listing describes it, in the terms of a compare.
     */
    ComparedState comparedStateOf(DirectoryEntry const& entry);

    /**
     * @brief True when @p current is still what the compare saw in @p compared. A path seen absent must still be
     *        absent. One seen present may be gone now, nothing there can be lost, unless @p compared says it must
     *        still exist. Otherwise the type must be the same, a symlink its literal, a regular file or a special one
     *        its size and modification time; a directory changes by its type only.
     */
    bool unchangedSinceCompare(ComparedState const& compared, ComparedState const& current);
}
