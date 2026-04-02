#pragma once

#include <persistence/state_core.hpp>
#include <shared_data/theme.hpp>
#include <constants/persistence.hpp>

#include <map>
#include <set>
#include <string>

namespace Persistence
{
    struct UiOptions : public DefaultMissingMember
    {
        std::string theme{Constants::defaultThemeName};
        SharedData::DarkLightMode darkLightMode{SharedData::DarkLightMode::System};
        bool showHiddenFilesLocally{false};
        bool showHiddenFilesRemotely{false};
        bool fileGridPathBarOnTop{false};
        std::set<std::string> neverShowAgainDialogs{};
        std::map<std::string /*extension*/, std::string /*assetPath*/> fileGridExtensionIcons{
            {".cpp", "icons/Development/noun-c-4921443.png"},
            {".hpp", "icons/Development/noun-c-4921443.png"},
            {".cxx", "icons/Development/noun-c-4921443.png"},
            {".hxx", "icons/Development/noun-c-4921443.png"},
            {".tpp", "icons/Development/noun-c-4921443.png"},
            {".c", "icons/Development/noun-c-file-115671.png"},
            {".h", "icons/Development/noun-c-file-115671.png"},
            {".cc", "icons/Development/noun-c-4921443.png"},
            {".hh", "icons/Development/noun-c-4921443.png"},
            {".js", "icons/Development/noun-js-4921450.png"},
            {".html", "icons/Development/noun-html-1174764.png"},
            {".css", "icons/Development/css-3-logo.png"},
            {".java", "icons/Development/noun-java-1156842.png"},
            {".jar", "icons/Development/noun-jar-4921452.png"},
            {".cs", "icons/Development/noun-csharp-4921443.png"},
            {".log", "icons/Development/noun-log-4921382.png"},
            {".sqlite", "icons/Development/sqlite.png"},
            {".rs", "icons/Development/rust.png"},
            {".swift", "icons/Development/swift.png"},
            {".py", "icons/Development/python.png"},
        };
    };
    BOOST_DESCRIBE_STRUCT(
        UiOptions,
        (),
        (theme,
            darkLightMode,
            showHiddenFilesLocally,
            showHiddenFilesRemotely,
            fileGridPathBarOnTop,
            neverShowAgainDialogs,
            fileGridExtensionIcons)
    )
}