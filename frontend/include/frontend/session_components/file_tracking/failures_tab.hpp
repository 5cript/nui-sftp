#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The Failed tab: an itemListBox counting item->failureCount, with Retry All (data-ft-action=retry-all,
     *        Primary) and Dismiss All (dismiss-all), the hint notRetriedAutomatically and the empty text noFailures.
     *        One row per item->failures (data-ft-failure-id, data-ft-reason=<UploadFailureReason>): the file, the
     *        reason (its title the detail when there is one, plus failedTimes after more than one attempt), the time,
     *        Retry (retry, actions.retryFailure) and a dismiss icon button (dismiss, title dismiss, aria-label
     *        dismissFailureLabel, actions.dismissFailure).
     */
    Nui::ElementRenderer
    failuresTab(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
