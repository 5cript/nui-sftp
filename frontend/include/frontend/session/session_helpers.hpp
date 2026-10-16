#pragma once

#include <frontend/session_components/numbered_saves.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace SessionInternal
{
    /**
     * @brief Resolves the glyph that represents a terminal's flavour in the
     *        per-terminal toolbar.  Named icon wins when supplied; otherwise a
     *        flavour-appropriate fallback is used.
     */
    Nui::ElementRenderer resolveIdentityIcon(std::string const& iconName, bool isLocalShell);

    /**
     * @brief Writes @p content to @p filePath, which the user picked in a save dialog that confirmed replacing it.
     */
    void writeChannelContentToFile(std::filesystem::path const& filePath, std::string const& content);

    /**
     * @brief Writes @p content to the new file @p filePath, an existing file is never replaced.
     */
    void writeChannelContentToNewFile(
        std::filesystem::path const& filePath,
        std::string const& content,
        std::function<void(WriteOutcome)> onDone
    );
}
