#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The head of the watch detail: the kind icon, the name, the kind label
     *        (fileExplorer.contextMenu.openAndWatch for copies, uploadAndWatch for originals), the status badge
     *        (data-ft-status=<WatchStatus>, text item->statusText) and the actions. A running watch offers Open Local
     *        Item (data-ft-action=open-local, actions.openLocally), Reveal on Server (reveal-on-server,
     *        actions.revealOnServer) and Stop (stop, actions.stop); a stopped one Open Local Item and Remove (remove,
     *        actions.removeStopped). Below, a muted line "local path -> remote path", the local part temporaryCopy for
     *        copies.
     */
    Nui::ElementRenderer
    watchHeader(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
