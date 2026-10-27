#pragma once

#include <filesystem>

namespace Utility
{
    /**
     * @brief @p path in the extended-length form on Windows (\\?\C:\x or \\?\UNC\server\share\x), which MAX_PATH does
     *        not limit. The path is normalized first, the extended-length form skips the normalization of separators,
     *        "." and "..". Unchanged for a relative path, one in the device namespace already and on other platforms.
     */
    std::filesystem::path extendedLengthPath(std::filesystem::path const& path);

    /**
     * @brief Whether @p path is in the extended-length or the device namespace (\\?\ or \\.\), always false on other
     *        platforms than Windows.
     */
    bool isExtendedLengthPath(std::filesystem::path const& path);

    /**
     * @brief @p path as users write it, without the prefix @ref extendedLengthPath added.
     */
    std::filesystem::path withoutExtendedLengthPrefix(std::filesystem::path const& path);
}
