#pragma once

#include <filesystem>

namespace Utility
{
    /**
     * @brief Whether renaming @p oldPath to @p newPath only changes the name: same parent directory and a new name
     *        that is a plain path segment. Anything else would move the item elsewhere.
     */
    inline bool isRenameInPlace(std::filesystem::path const& oldPath, std::filesystem::path const& newPath)
    {
        const auto newName = newPath.filename();
        if (newName.empty() || newName == "." || newName == "..")
            return false;
        return !oldPath.parent_path().empty() && newPath.parent_path() == oldPath.parent_path();
    }
}
