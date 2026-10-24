#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief Why a watch stopped: a messageStrip (data-ft-reason=<WatchStopReason>) with item->stopText, mounted always
     * and hidden while the watch runs. Its actions slot holds Show “Y” (data-ft-action=show-other-watch, text
     *        showOtherWatch, actions.showWatch) for an Overlap stop that names the other watch, and Open Again
     *        (open-again, actions.openAgain) for any other stopped copy. Stopped originals get no action.
     */
    Nui::ElementRenderer
    stopReasonStrip(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
