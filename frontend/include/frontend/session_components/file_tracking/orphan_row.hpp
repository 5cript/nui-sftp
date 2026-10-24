#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The outliner row of an orphaned copy: a listRow (data-ft-instance-id, data-ft-name) led by orphanGlyph,
     *        with the size item->sideText beside it and a trash button (data-ft-action=delete-orphan, title
     *        deleteOrphanTooltip) calling actions.deleteOrphanedCopy. Selected while store.selection() holds its
     *        instance; activating it calls actions.select.
     */
    Nui::ElementRenderer
    orphanRow(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<OrphanItem> const& item);
}
