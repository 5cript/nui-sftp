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
        // Items per page in the file grid pagination footer. The user prefers unpaginated UX,
        // so the default is intentionally large; the footer hides itself when the directory
        // (or filtered match set) fits within one page.
        int fileGridPageSize{500};
        std::set<std::string> neverShowAgainDialogs{};
        std::vector<std::string> localFavorites{};
        // Whether the first-launch onboarding flow has been completed
        // (finished or dismissed). Set once; never auto-reset.
        bool onboardingCompleted{false};
        std::map<std::string /*extension*/, std::string /*assetPath*/> fileGridExtensionIcons{
            {".cpp", "icons/masks/Development/noun-c-4921443.png"},
            {".hpp", "icons/masks/Development/noun-c-4921443.png"},
            {".cxx", "icons/masks/Development/noun-c-4921443.png"},
            {".hxx", "icons/masks/Development/noun-c-4921443.png"},
            {".tpp", "icons/masks/Development/noun-c-4921443.png"},
            {".c", "icons/masks/Development/noun-c-file-115671.png"},
            {".h", "icons/masks/Development/noun-c-file-115671.png"},
            {".cc", "icons/masks/Development/noun-c-4921443.png"},
            {".hh", "icons/masks/Development/noun-c-4921443.png"},
            {".js", "icons/masks/Development/noun-js-4921450.png"},
            {".html", "icons/masks/Development/noun-html-1174764.png"},
            {".css", "icons/masks/Development/css-3-logo.png"},
            {".java", "icons/masks/Development/noun-java-1156842.png"},
            {".jar", "icons/masks/Development/noun-jar-4921452.png"},
            {".cs", "icons/masks/Development/noun-code-4641601.png"},
            {".log", "icons/masks/Development/noun-log-4921382.png"},
            {".sqlite", "icons/masks/Development/noun-sql-4921378.png"},
            {".rs", "icons/masks/Development/Rust_programming_language.png"},
            {".swift", "icons/masks/Development/noun-swift-file-3001056.png"},
            {".py", "icons/masks/Development/noun-python-1375869.png"},
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
            fileGridPageSize,
            neverShowAgainDialogs,
            localFavorites,
            onboardingCompleted,
            fileGridExtensionIcons)
    )
}