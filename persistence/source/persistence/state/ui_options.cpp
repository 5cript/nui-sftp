#include <persistence/state/ui_options.hpp>

#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>

using namespace std::string_view_literals;

namespace Persistence
{
    namespace
    {
        constexpr std::string_view genericFileIcon = "icons/file.svg";

        /**
         * @brief Icon file names (without extension) of earlier versions and the icon that replaces them.
         */
        std::unordered_map<std::string_view, std::string_view> const& replacedIcons()
        {
            static const std::unordered_map<std::string_view, std::string_view> replaced{
                {"cpp", "icons/file-types/cplusplus.svg"},
                {"noun-c-4921443", "icons/file-types/cplusplus.svg"},
                {"c", "icons/file-types/c.svg"},
                {"noun-c-file-115671", "icons/file-types/c.svg"},
                {"js", "icons/file-types/javascript.svg"},
                {"noun-js-4921450", "icons/file-types/javascript.svg"},
                {"html", "icons/file-types/html5.svg"},
                {"noun-html-1174764", "icons/file-types/html5.svg"},
                {"css", "icons/file-types/css.svg"},
                {"css-3-logo", "icons/file-types/css.svg"},
                {"java", "icons/file-types/openjdk.svg"},
                {"noun-java-1156842", "icons/file-types/openjdk.svg"},
                {"jar", "icons/file-types/openjdk.svg"},
                {"noun-jar-4921452", "icons/file-types/openjdk.svg"},
                {"csharp", "icons/file-types/file-code.svg"},
                {"noun-csharp-4921443", "icons/file-types/file-code.svg"},
                {"noun-code-4641601", "icons/file-types/file-code.svg"},
                {"log", genericFileIcon},
                {"noun-log-4921382", genericFileIcon},
                {"file", genericFileIcon},
                {"sqlite", "icons/file-types/sqlite.svg"},
                {"noun-sql-4921378", "icons/file-types/sqlite.svg"},
                {"rust", "icons/file-types/rust.svg"},
                {"Rust_programming_language", "icons/file-types/rust.svg"},
                {"swift", "icons/file-types/swift.svg"},
                {"noun-swift-file-3001056", "icons/file-types/swift.svg"},
                {"python", "icons/file-types/python.svg"},
                {"noun-python-1375869", "icons/file-types/python.svg"},
            };
            return replaced;
        }

        /**
         * @brief The icon file name without extension if the path has the layout of an earlier version.
         * @param iconPath An asset path like "icons/masks/Development/noun-c-4921443.png".
         */
        std::optional<std::string_view> outdatedIconName(std::string_view iconPath)
        {
            constexpr std::array outdatedDirectories{"icons/masks/Development/"sv, "icons/Development/"sv, "icons/"sv};
            for (const auto directory : outdatedDirectories)
            {
                if (!iconPath.starts_with(directory))
                    continue;
                const auto fileName = iconPath.substr(directory.size());
                if (fileName.find('/') != std::string_view::npos)
                    continue;
                return fileName.substr(0, fileName.rfind('.'));
            }
            return std::nullopt;
        }

        /**
         * @brief The icon that replaces an outdated icon path, if any.
         * @param iconPath The stored asset path.
         */
        std::optional<std::string_view> replacementIconPath(std::string_view iconPath)
        {
            if (const auto name = outdatedIconName(iconPath))
            {
                if (const auto replaced = replacedIcons().find(*name); replaced != replacedIcons().end())
                    return replaced->second;
            }
            // Everything else from the removed icon bundle no longer exists.
            if (iconPath.starts_with("icons/masks/") || iconPath.starts_with("icons/os_folders/") ||
                iconPath.starts_with("icons/Development/"))
                return genericFileIcon;
            return std::nullopt;
        }
    }

    bool updateOutdatedExtensionIconPaths(UiOptions& uiOptions)
    {
        bool updated = false;
        for (auto& [extension, iconPath] : uiOptions.fileGridExtensionIcons)
        {
            const auto replacement = replacementIconPath(iconPath);
            if (!replacement || *replacement == iconPath)
                continue;
            iconPath = std::string{*replacement};
            updated = true;
        }
        return updated;
    }
}
