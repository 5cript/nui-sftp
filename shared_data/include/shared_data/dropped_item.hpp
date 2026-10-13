#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/directory_entry.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <nlohmann/json.hpp>

namespace SharedData
{
    /**
     * @brief A local file or directory dropped onto the window from the desktop.
     */
    struct DroppedItem
    {
        Paths::LocalPath path{};
        /** @brief Type and name of the item, enough for a transfer. */
        DirectoryEntry entry{};
    };
    BOOST_DESCRIBE_STRUCT(DroppedItem, (), (path, entry))
}
