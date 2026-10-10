#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Utility
{
    /**
     * @brief One entry of an XDG user-dirs.dirs file, such as XDG_DESKTOP_DIR.
     */
    struct UserDirectory
    {
        /**
         * @brief The name between "XDG_" and "_DIR", such as "DESKTOP" or "DOWNLOAD".
         */
        std::string name;

        std::filesystem::path path;
    };

    /**
     * @brief Parses the content of a user-dirs.dirs file. Entries that point at the home directory itself are disabled
     * by the XDG specification and left out, as are lines that do not follow the format.
     *
     * @param content The content of the file.
     * @param home The home directory that $HOME expands to.
     */
    std::vector<UserDirectory> parseUserDirectories(std::string_view content, std::filesystem::path const& home);
}
