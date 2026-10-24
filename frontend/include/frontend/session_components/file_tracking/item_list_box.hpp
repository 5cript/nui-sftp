#pragma once

#include <script-nui-components/style_variant.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The options of @ref itemListBox, for designated initializers in this order. Every field has a default.
     */
    struct ItemListBoxOptions
    {
        /** @brief The buttons at the start of the toolbar above the box. */
        std::optional<Nui::ElementRenderer> actions = std::nullopt;
        /** @brief A muted hint at the end of the toolbar. */
        std::string hint = {};
        /** @brief Shown instead of the toolbar and the box while @ref count is 0. */
        std::string emptyText = {};
        /** @brief The number of items, data-empty is on the root while it is 0. A null pointer counts as not empty. */
        std::shared_ptr<Nui::Observed<std::uint64_t>> count = {};
        /** @brief Merged onto the root, for example data-class="conflicts" or "failures" for the box colours. */
        std::vector<Nui::Attribute> attributes = {};
    };

    /**
     * @brief A toolbar above a box that scrolls on its own, or an empty text instead of both. Markup:
     *        div.file-tracking-item-list-box[data-empty] > div.file-tracking-item-list-box-toolbar
     *        (div.file-tracking-item-list-box-actions holding the actions, then span.file-tracking-item-list-box-hint),
     *        div.file-tracking-item-list-box-items (@p content) and div.file-tracking-item-list-box-empty (emptyText).
     *        Kept in the host until a second user (the queue's Failed tab) adopts it.
     * @param content The rows, usually an element holding a Nui::range.
     */
    Nui::ElementRenderer itemListBox(ItemListBoxOptions options, Nui::ElementRenderer content);

    /**
     * @brief The options of @ref itemListBoxButton, for designated initializers in this order.
     */
    struct ItemListBoxButtonOptions
    {
        std::string text = {};
        std::optional<Nui::ElementRenderer> icon = std::nullopt;
        /** @brief The value of data-ft-action. */
        std::string action = {};
        ScriptNuiComponents::StyleVariant styleVariant = ScriptNuiComponents::StyleVariant::Regular;
        std::function<void()> onPress = {};
        /** @brief Merged onto the button, for a title or an aria-label. */
        std::vector<Nui::Attribute> attributes = {};
    };

    /**
     * @brief A small library button for the toolbar or the rows of an @ref itemListBox.
     */
    Nui::ElementRenderer itemListBoxButton(ItemListBoxButtonOptions options);
}
