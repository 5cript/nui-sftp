#pragma once

#include <frontend/session_components/file_tracking/tracking_actions.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <memory>

namespace FileTracking
{
    /**
     * @brief The pinned progress of a watch (aria-label progressLabel). While Syncing: progressFiles and
     * progressWaiting (plus progressWaitingForAnswer while answers are due), a total progressBar in Bytes mode on
     * item->bytesDone and item->bytesTotal (data-ft-progress-total), and per row of item->inFlight its name and a small
     * progressBar (data-ft-progress-file=<local path>). Otherwise a summary line (statusUpToDate,
     * progressUpToDateLastUpload, progressStoppedLastUpload, progressConflictsWait(One) or progressFilesUpToDate) and,
     * after a sync, a bar of the last sync.
     */
    Nui::ElementRenderer
    syncProgressBlock(TrackingStore& store, TrackingActions const& actions, std::shared_ptr<WatchItem> const& item);
}
