#pragma once

#include <shared_data/dropped_item.hpp>

#include <filesystem>
#include <optional>

/**
 * @brief Describes a file dropped onto the window from the desktop, with just enough detail for the transfer.
 * @param path Local path of the dropped file or directory, nothing when it is not an absolute path.
 */
std::optional<SharedData::DroppedItem> droppedItemFromPath(std::filesystem::path const& path);
