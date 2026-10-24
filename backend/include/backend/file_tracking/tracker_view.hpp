#pragma once

#include <ids/ids.hpp>
#include <shared_data/file_tracking/ended_watches.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/server_changed_question.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/tracker_progress.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>

#include <cstddef>

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
         * @brief Asks @p question, the tracker waits for the answer before it asks the next one.
         */
        virtual void ask(SharedData::ServerChangedQuestion const& question) = 0;

        /**
         * @brief Shows how many instance directories no live process holds any more.
         */
        virtual void showOrphanedCount(std::size_t count) = 0;

        /**
         * @brief Shows how far the tracker is with the changes on their way, at most a few times a second.
         */
        virtual void showProgress(SharedData::TrackerProgress const& progress) = 0;

        /**
         * @brief Shows @p conflict, a local change that waits for the user's answer.
         */
        virtual void showConflict(SharedData::WatchConflict const& conflict) = 0;

        /**
         * @brief The conflict @p conflictId was answered or dropped with its watch.
         */
        virtual void removeConflict(Ids::ConflictId const& conflictId) = 0;

        /**
         * @brief Shows @p failure, new or updated.
         */
        virtual void showFailure(SharedData::UploadFailure const& failure) = 0;

        /**
         * @brief The failure @p failureId was retried, dismissed, went through or was dropped with its watch.
         */
        virtual void removeFailure(Ids::FailureId const& failureId) = 0;

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
