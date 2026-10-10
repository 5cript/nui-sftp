#pragma once

#include <shared_data/directory_entry.hpp>

#include <filesystem>

/**
 * @brief Describes a file dropped onto the window from the desktop, with just enough detail for the transfer.
 * @param path Local path of the dropped file or directory.
 */
SharedData::DirectoryEntry leanDirectoryEntryFromPath(std::filesystem::path const& path);
