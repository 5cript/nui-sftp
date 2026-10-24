#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

namespace FileTracking
{
    /**
     * @brief The master list of the panel: the filter textInput (data-ft-filter, placeholder filterPlaceholder) above a
     *        role=listbox (aria-label outlinerLabel) holding one collapsibleSection per group of outlinerGroupOrder
     *        (data-ft-group=<OutlinerGroup>). Each section lists store.membersOf(group) with watchRow, Orphaned lists
     *        store.orphanMembers() with orphanRow. Its headerEnd holds countBadge(store.countOf(group)); Stopped adds a
     *        trash button (data-ft-action=remove-all-stopped, actions.removeAllStopped) and Orphaned one
     *        (data-ft-action=delete-all-orphans, actions.deleteAllOrphanedCopies). A group without members stays
     * mounted and gets data-hidden. Up, Down, Home and End on the list call actions.moveSelection; folding a group
     * calls actions.setGroupExpanded; typing calls actions.filter.
     */
    Nui::ElementRenderer outliner(TrackingStore& store, TrackingActions const& actions);
}
