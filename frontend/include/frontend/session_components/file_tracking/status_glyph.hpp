#pragma once

#include <frontend/session_components/file_tracking/panel_state.hpp>

#include <nui/frontend/element_renderer.hpp>

namespace FileTracking
{
    /**
     * @brief The icon that leads an outliner row that shows no ring: a check for Watching, a question for Conflicts, a
     *        warning triangle for Failed, a square for Stopped (Syncing, normally shown as a ring, gets the check). A
     *        span.file-tracking-glyph[data-status=<WatchStatus>] around a Ui5 icon, coloured by CSS.
     */
    Nui::ElementRenderer statusGlyph(WatchStatus status);

    /**
     * @brief The file icon that leads an orphan row, span.file-tracking-glyph[data-status=Orphaned].
     */
    Nui::ElementRenderer orphanGlyph();
}
