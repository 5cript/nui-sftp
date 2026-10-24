#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The detail pane of an orphaned copy (data-ft-detail=orphan, data-ft-instance-id): its glyph, name and kind
     *        label orphanKind, the actions Show Folder (data-ft-action=show-folder, actions.openLocally on its
     * directory) and Delete Copy (delete-orphan, Danger, actions.deleteOrphanedCopy, which confirms first), the lines
     *        orphanDownloadedFrom or orphanOriginUnknown, orphanDownloadedAt, orphanEndedAt and orphanSize, and
     *        orphanExplanation.
     */
    Nui::ElementRenderer
    orphanDetail(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<OrphanItem> const& item);
}
