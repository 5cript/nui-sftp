#pragma once

#include <array>
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
     * @brief A default place of the file explorer, found through user-dirs.dirs or by its usual folder in home.
     */
    struct DefaultPlace
    {
        /**
         * @brief Stable id to pick icons and translations by, such as "downloads".
         */
        std::string_view kind;

        /**
         * @brief English label.
         */
        std::string_view name;

        /**
         * @brief The name in user-dirs.dirs, such as DESKTOP for XDG_DESKTOP_DIR. Empty when XDG has none.
         */
        std::string_view userDirectoryName;

        /**
         * @brief The usual folder relative to home.
         */
        std::string_view directoryName;
    };

    inline constexpr std::array<DefaultPlace, 7> defaultPlaces{{
        {"desktop", "Desktop", "DESKTOP", "Desktop"},
        {"downloads", "Downloads", "DOWNLOAD", "Downloads"},
        {"documents", "Documents", "DOCUMENTS", "Documents"},
        {"pictures", "Pictures", "PICTURES", "Pictures"},
        {"videos", "Videos", "VIDEOS", "Videos"},
        {"movies", "Movies", "", "Movies"},
        {"music", "Music", "MUSIC", "Music"},
    }};

    /**
     * @brief Parses the content of a user-dirs.dirs file. Entries that point at the home directory itself are disabled
     * by the XDG specification and left out, as are lines that do not follow the format.
     *
     * @param content The content of the file.
     * @param home The home directory that $HOME expands to.
     */
    std::vector<UserDirectory> parseUserDirectories(std::string_view content, std::filesystem::path const& home);
}
