#pragma once

#include <utility/keyed_diff.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/attributes/impl/attribute.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    /**
     * @brief Brings an observed list to @p target touching only the elements that differ, so a range
     *        bound to it redraws just those. When most of it changes anyway, it is replaced at once.
     *
     * The caller syncs the event context, as after any other change.
     *
     * @param key Maps an element to its key; keys must be unique within the list.
     */
    template <typename T, typename KeyFunction>
    void updateKeyed(Nui::Observed<std::vector<T>>& observed, std::vector<T> target, KeyFunction&& key)
    {
        const auto edits = Utility::keyedEdits(observed.value(), target, key);
        if (edits.empty())
            return;
        if (edits.size() * 2 > std::max(observed.value().size(), target.size()))
        {
            observed = std::move(target);
            return;
        }

        for (auto const& edit : edits)
        {
            const auto position = static_cast<std::ptrdiff_t>(edit.index);
            switch (edit.kind)
            {
                case Utility::KeyedEdit::Kind::Erase:
                    observed.erase(observed.cbegin() + position);
                    break;
                case Utility::KeyedEdit::Kind::Insert:
                    observed.insert(observed.cbegin() + position, target[edit.source]);
                    break;
                case Utility::KeyedEdit::Kind::Update:
                    observed[edit.index] = target[edit.source];
                    break;
            }
        }
    }
}
