#pragma once

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <cstdint>
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
     *        div.file-tracking-item-list-box[data-empty] > div.file-tracking-item-list-box-toolbar (actions, then
     *        span.file-tracking-item-list-box-hint), div.file-tracking-item-list-box-items (@p content) and
     *        div.file-tracking-item-list-box-empty (emptyText). Kept in the host until a second user (the queue's
     *        Failed tab) adopts it.
     * @param content The rows, usually an element holding a Nui::range.
     */
    Nui::ElementRenderer itemListBox(ItemListBoxOptions options, Nui::ElementRenderer content);
}
