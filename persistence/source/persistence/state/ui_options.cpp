#include <persistence/state/ui_options.hpp>

#include <string_view>
#include <unordered_map>

namespace Persistence
{
    namespace
    {
        constexpr std::string_view outdatedDevelopmentDirectory = "icons/Development/";
        constexpr std::string_view developmentDirectory = "icons/masks/Development/";

        std::unordered_map<std::string_view, std::string_view> const& renamedIcons()
        {
            static const std::unordered_map<std::string_view, std::string_view> renamed{
                {"icons/cpp.png", "noun-c-4921443.png"},
                {"icons/c.png", "noun-c-file-115671.png"},
                {"icons/js.png", "noun-js-4921450.png"},
                {"icons/html.png", "noun-html-1174764.png"},
                {"icons/css.png", "css-3-logo.png"},
                {"icons/java.png", "noun-java-1156842.png"},
                {"icons/jar.png", "noun-jar-4921452.png"},
                {"icons/csharp.png", "noun-code-4641601.png"},
                {"icons/log.png", "noun-log-4921382.png"},
                {"icons/sqlite.png", "noun-sql-4921378.png"},
                {"icons/rust.png", "Rust_programming_language.png"},
                {"icons/swift.png", "noun-swift-file-3001056.png"},
                {"icons/python.png", "noun-python-1375869.png"},
                {"icons/Development/noun-csharp-4921443.png", "noun-code-4641601.png"},
                {"icons/Development/sqlite.png", "noun-sql-4921378.png"},
                {"icons/Development/rust.png", "Rust_programming_language.png"},
                {"icons/Development/swift.png", "noun-swift-file-3001056.png"},
                {"icons/Development/python.png", "noun-python-1375869.png"},
            };
            return renamed;
        }
    }

    bool updateOutdatedExtensionIconPaths(UiOptions& uiOptions)
    {
        bool updated = false;
        for (auto& [extension, iconPath] : uiOptions.fileGridExtensionIcons)
        {
            if (const auto renamed = renamedIcons().find(iconPath); renamed != renamedIcons().end())
                iconPath = std::string{developmentDirectory} + std::string{renamed->second};
            else if (iconPath.starts_with(outdatedDevelopmentDirectory))
                iconPath = std::string{developmentDirectory} + iconPath.substr(outdatedDevelopmentDirectory.size());
            else
                continue;
            updated = true;
        }
        return updated;
    }
}
