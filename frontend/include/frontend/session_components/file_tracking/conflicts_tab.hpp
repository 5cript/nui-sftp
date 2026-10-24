#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The Conflicts tab: an itemListBox counting item->conflictCount, with Replace All on Server
     *        (data-ft-action=replace-all, Warning) and Keep All Server's (keep-all) calling actions.answerAllConflicts,
     * the hint keepAsksAgain and the empty text noConflicts. One row per item->conflicts (data-ft-conflict-id): the
     *        file, the line, Replace on Server (replace, Warning) and Keep Server's (keep) calling
     * actions.answerConflict.
     */
    Nui::ElementRenderer
    conflictsTab(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
