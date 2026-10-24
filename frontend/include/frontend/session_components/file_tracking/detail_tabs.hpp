#pragma once

#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <script-nui-components/style_variant.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief One tab of the watch detail. A new tab (Activity, later) is one more registry entry.
     */
    struct DetailTabEntry
    {
        DetailTab tab{DetailTab::Conflicts};
        /** @brief The translated title, from a literal language key (tabConflicts, tabFailed, tabOptions). */
        std::string title{};
        /** @brief The count of the tab's badge for an item, nullptr or an empty function for no badge. */
        std::function<std::shared_ptr<Nui::Observed<std::uint64_t>>(WatchItem const& item)> count{};
        ScriptNuiComponents::StyleVariant badgeVariant{ScriptNuiComponents::StyleVariant::Regular};
        /** @brief Renders the tab's panel. */
        std::function<Nui::ElementRenderer(
            TrackingStore& store,
            TrackingActions const& actions,
            std::shared_ptr<WatchItem> const& item
        )>
            render{};
    };

    /**
     * @return The tabs in detailTabOrder: Conflicts (count item.conflictCount, Warning, conflictsTab), Failed
     *         (item.failureCount, Danger, failuresTab) and Options (no badge, optionsTab).
     */
    std::vector<DetailTabEntry> detailTabRegistry();

    /**
     * @brief The tabStrip of the registry (data-tab-key=<DetailTab>, aria-label detailTabsLabel) on
     *        store.detailTabKey(), calling actions.chooseTab, above the panel of the chosen tab. Tabs are the same
     *        for every watch and the chosen one stays when the selection changes.
     */
    Nui::ElementRenderer
    detailTabs(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
