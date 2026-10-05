#pragma once

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/attributes/impl/attribute.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief Small helpers shared by the command history and snippet panels.
 */
namespace CommandPanels
{
    /**
     * @brief Lowercases ASCII characters; multibyte sequences pass through unchanged.
     */
    std::string lowercased(std::string_view text);

    /**
     * @brief True when @p text contains @p loweredQuery or matches it fuzzily.
     *
     * @param loweredQuery The search input, already lowercased; empty matches everything.
     */
    bool matchesSearch(std::string const& loweredQuery, std::string const& text);

    /**
     * @brief Short human readable age like "5 min ago", translated.
     */
    std::string relativeTime(std::int64_t epochSeconds, std::int64_t nowEpoch);

    /**
     * @brief Renders @p text in a span, wrapping a plain substring match of the query in a mark.
     *
     * Fuzzy-only matches render without a mark; the query being empty renders plain text.
     *
     * @param cssClass Class of the surrounding span.
     */
    Nui::ElementRenderer
    highlightedText(std::string const& text, std::string const& loweredQuery, std::string const& cssClass);

    /**
     * @brief Disables a button that acts on the terminal while the session's connection is lost.
     */
    Nui::Attribute disabledWhileDisconnected(Nui::Observed<bool>& connectionLost);

    /**
     * @brief The button's tooltip, replaced by the reason it is disabled while the connection is lost.
     */
    Nui::Attribute connectionTooltip(Nui::Observed<bool>& connectionLost, std::string tooltip);
}
