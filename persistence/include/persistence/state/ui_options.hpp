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
            {".cpp", "icons/file-types/cplusplus.svg"},
            {".hpp", "icons/file-types/cplusplus.svg"},
            {".cxx", "icons/file-types/cplusplus.svg"},
            {".hxx", "icons/file-types/cplusplus.svg"},
            {".tpp", "icons/file-types/cplusplus.svg"},
            {".c", "icons/file-types/c.svg"},
            {".h", "icons/file-types/c.svg"},
            {".cc", "icons/file-types/cplusplus.svg"},
            {".hh", "icons/file-types/cplusplus.svg"},
            {".js", "icons/file-types/javascript.svg"},
            {".html", "icons/file-types/html5.svg"},
            {".css", "icons/file-types/css.svg"},
            {".java", "icons/file-types/openjdk.svg"},
            {".jar", "icons/file-types/openjdk.svg"},
            {".cs", "icons/file-types/file-code.svg"},
            {".log", "icons/file.svg"},
            {".sqlite", "icons/file-types/sqlite.svg"},
            {".rs", "icons/file-types/rust.svg"},
            {".swift", "icons/file-types/swift.svg"},
            {".py", "icons/file-types/python.svg"},
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

    /**
     * @brief Rewrites file type icon paths of earlier versions to the icons shipped now.
     * @param uiOptions Options whose fileGridExtensionIcons are updated in place.
     * @return Whether any path was rewritten.
     */
    bool updateOutdatedExtensionIconPaths(UiOptions& uiOptions);
}