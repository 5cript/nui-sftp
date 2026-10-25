#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The Options tab: one switch_ row per SharedData::WatchOption (data-ft-option=<WatchOption>) with its label
     *        (columnAutoReupload, columnMoveRemote, columnDeleteRemote, columnCheckRemote) and description
     *        (option...Description), calling actions.setOption. On a copy MoveRemoteOnLocalMove and
     *        DeleteRemoteOnLocalDelete are off and disabled, described by optionNeverForCopy with the title
     *        downloadedCopyOptionHint, which also shows below the switches.
     */
    Nui::ElementRenderer
    optionsTab(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
