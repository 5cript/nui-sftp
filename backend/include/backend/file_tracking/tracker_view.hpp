#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_tracking/ended_watches.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/removed_problems.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>

namespace FileTracking
{
    /**
     * @brief What the user sees of a Tracker: its entries, their conflicts, failures and progress, and the orphaned
     *        copies. Called on the strand of the tracker.
     */
    class TrackerView
    {
      public:
        TrackerView() = default;
        virtual ~TrackerView() = default;
        TrackerView(TrackerView const&) = delete;
        TrackerView& operator=(TrackerView const&) = delete;
        TrackerView(TrackerView&&) = delete;
        TrackerView& operator=(TrackerView&&) = delete;

        /**
         * @brief Shows @p entry, new or changed, a stopped one too.
         */
        virtual void showEntry(SharedData::TrackedEntry const& entry) = 0;

        virtual void removeEntry(Ids::WatchId const& watchId) = 0;

        /**
         * @brief Tells the user why the watches of @p ended ended, they did not stop them. Their entries are removed
         *        before, see removeEntry().
         */
        virtual void showEnded(SharedData::EndedWatches const& ended) = 0;

        /**
         * @brief Shows @p conflict, a local change that waits for the user's answer.
         */
        virtual void showConflict(SharedData::WatchConflict const& conflict) = 0;

        /**
         * @brief The conflicts of @p removed were answered or dropped with their watch, all in one message.
         */
        virtual void removeConflicts(SharedData::RemovedConflicts const& removed) = 0;

        /**
         * @brief Shows @p failure, new or updated.
         */
        virtual void showFailure(SharedData::UploadFailure const& failure) = 0;

        /**
         * @brief The failures of @p removed were retried, dismissed, went through or were dropped with their watch, all
         *        in one message.
         */
        virtual void removeFailures(SharedData::RemovedFailures const& removed) = 0;

        /**
         * @brief Shows how far the watch of @p progress is, at most five times a second per watch.
         */
        virtual void showWatchProgress(SharedData::WatchProgress const& progress) = 0;

        /**
         * @brief Shows the orphaned copies, all of them, replacing what was shown before.
         */
        virtual void showOrphanedCopies(SharedData::OrphanedCopies const& copies) = 0;
    };
}
