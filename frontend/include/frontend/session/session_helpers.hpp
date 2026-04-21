#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <filesystem>
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
     * @brief Fires the RPC that writes @p content to @p filePath, with
     *        user-facing logging consistent with the multi-channel save path.
     */
    void writeChannelContentToFile(std::filesystem::path const& filePath, std::string const& content);
}
