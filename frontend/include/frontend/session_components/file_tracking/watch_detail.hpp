#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The detail pane of a watch (data-ft-detail=watch, data-ft-watch-id): watchHeader, stopReasonStrip,
     *        syncProgressBlock and detailTabs, top to bottom.
     */
    Nui::ElementRenderer
    watchDetail(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
