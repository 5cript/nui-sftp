#pragma once

#include <shared_data/dropped_item.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <vector>

/**
 * @brief Describes a file dropped onto the window from the desktop, with just enough detail for the transfer. A
 *        dropped link or junction is a symlink, never what it points to.
 * @param path Local path of the dropped item. Nothing, and a log line, when it is not an absolute path or cannot be
 *        inspected.
 */
std::optional<SharedData::DroppedItem> droppedItemFromPath(std::filesystem::path const& path);

/**
 * @brief The SessionArea::onFilesDropped message for files the frontend handed to the backend.
 * @param request The frontend's message, its isLeft, subdir, copy and dropMetadata go back with the files.
 * @param entries The dropped files.
 */
nlohmann::json droppedFilesMessage(nlohmann::json const& request, std::vector<SharedData::DroppedItem> const& entries);
