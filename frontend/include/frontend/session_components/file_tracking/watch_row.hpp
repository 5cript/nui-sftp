#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The outliner row of a watch: a listRow (data-ft-watch-id, data-ft-name, data-ft-state=<WatchStatus>) led
     * by a progressRing on item->ringFraction while the watch is Syncing (label syncingTooltip), by statusGlyph
     *        otherwise. Its side text is item->sideText; it is selected while store.selection() holds its watch and
     *        activating it calls actions.select. A stopped row gets a trash button (data-ft-action=remove-stopped,
     *        title removeStoppedTooltip) calling actions.removeStopped.
     */
    Nui::ElementRenderer
    watchRow(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
