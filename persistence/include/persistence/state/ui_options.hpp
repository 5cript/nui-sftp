#pragma once

#include <persistence/state_core.hpp>
#include <shared_data/theme.hpp>
#include <constants/persistence.hpp>

#include <map>
#include <string>

namespace Persistence
{
    struct UiOptions : public DefaultMissingMember
    {
        std::string theme{Constants::defaultThemeName};
        SharedData::DarkLightMode darkLightMode{SharedData::DarkLightMode::System};
        bool showHiddenFiles{false};
        bool fileGridPathBarOnTop{false};
        std::map<std::string /*extension*/, std::string /*assetPath*/> fileGridExtensionIcons{
            {".cpp", "icons/cpp.png"},
            {".hpp", "icons/cpp.png"},
            {".cxx", "icons/cpp.png"},
            {".hxx", "icons/cpp.png"},
            {".tpp", "icons/cpp.png"},
            {".c", "icons/c.png"},
            {".h", "icons/c.png"},
            {".cc", "icons/cpp.png"},
            {".hh", "icons/cpp.png"},
            {".js", "icons/js.png"},
            {".html", "icons/html.png"},
            {".css", "icons/css.png"},
            {".java", "icons/java.png"},
            {".jar", "icons/jar.png"},
            {".cs", "icons/csharp.png"},
            {".log", "icons/log.png"},
            {".sqlite", "icons/sqlite.png"},
            {".rs", "icons/rust.png"},
            {".swift", "icons/swift.png"},
            {".py", "icons/python.png"},
        };
    };
    BOOST_DESCRIBE_STRUCT(
        UiOptions,
        (),
        (theme, darkLightMode, showHiddenFiles, fileGridPathBarOnTop, fileGridExtensionIcons)
    )
}