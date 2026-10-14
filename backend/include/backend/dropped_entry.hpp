#pragma once

#include <shared_data/dropped_item.hpp>

#include <filesystem>
#include <optional>

/**
 * @brief Describes a file dropped onto the window from the desktop, with just enough detail for the transfer. A
 *        dropped link or junction is a symlink, never what it points to.
 * @param path Local path of the dropped item. Nothing, and a log line, when it is not an absolute path or cannot be
 *        inspected.
 */
std::optional<SharedData::DroppedItem> droppedItemFromPath(std::filesystem::path const& path);
