#pragma once

#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <ids/ids.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Whether @p name passes the outliner filter: an empty filter passes everything, otherwise @p name has to
     *        contain @p filter, ASCII letters compared without case.
     */
    bool matchesFilter(std::string_view name, std::string_view filter);

    /**
     * @brief Everything the tracker of one session told the panel, and the rules that sort it into the outliner. Pure,
     *        tested natively; TrackingStore projects it onto observed values.
     */
    class TrackingModel
    {
      public:
        /**
         * @brief Adds the watch of @p entry or replaces what is known of it. Stopped entries have stop set.
         */
        void showEntry(SharedData::TrackedEntry const& entry);

        /**
         * @brief Forgets the watch @p watchId with its progress, conflicts and failures.
         */
        void removeEntry(Ids::WatchId const& watchId);

        /**
         * @brief Adds @p conflict or replaces the one with its id. Kept even while its watch is not known yet.
         */
        void showConflict(SharedData::WatchConflict const& conflict);

        /**
         * @brief Forgets the conflict @p conflictId, it was answered or dropped.
         */
        void removeConflict(Ids::ConflictId const& conflictId);

        /**
         * @brief Adds @p failure or replaces the one with its id (a repeated failure keeps its id and counts attempts).
         */
        void showFailure(SharedData::UploadFailure const& failure);

        /**
         * @brief Forgets the failure @p failureId, it went through, was retried or dismissed.
         */
        void removeFailure(Ids::FailureId const& failureId);

        /**
         * @brief Replaces the progress of the watch @p progress names.
         */
        void showWatchProgress(SharedData::WatchProgress const& progress);

        /**
         * @brief Replaces the list of orphaned copies.
         */
        void showOrphanedCopies(SharedData::OrphanedCopies const& copies);

        /**
         * @brief Forgets everything, the session ended.
         */
        void clear();

        /**
         * @return The entry of @p watchId, nullptr when it is not known.
         */
        SharedData::TrackedEntry const* entryOf(Ids::WatchId const& watchId) const;

        /**
         * @return The last progress of @p watchId, nullptr when none arrived.
         */
        SharedData::WatchProgress const* progressOf(Ids::WatchId const& watchId) const;

        /**
         * @return The orphaned copy @p instanceId, nullptr when it is not known or an entry still holds its instance.
         */
        SharedData::OrphanedCopy const* orphanOf(Ids::InstanceId const& instanceId) const;

        /**
         * @return The watch the conflict @p conflictId belongs to, nothing when the conflict is not known.
         */
        std::optional<Ids::WatchId> watchOfConflict(Ids::ConflictId const& conflictId) const;

        /**
         * @return The watch the failure @p failureId belongs to, nothing when the failure is not known.
         */
        std::optional<Ids::WatchId> watchOfFailure(Ids::FailureId const& failureId) const;

        /**
         * @return The conflicts of @p watchId, newest raisedAt first, ties by the greater conflict id first like
         *         @ref newestConflict.
         */
        std::vector<SharedData::WatchConflict> conflictsOf(Ids::WatchId const& watchId) const;

        /**
         * @return The failures of @p watchId, newest failedAt first, ties by the greater failure id first.
         */
        std::vector<SharedData::UploadFailure> failuresOf(Ids::WatchId const& watchId) const;

        /**
         * @return The number of conflicts of @p watchId.
         */
        std::uint64_t conflictCountOf(Ids::WatchId const& watchId) const;

        /**
         * @return The number of failures of @p watchId.
         */
        std::uint64_t failureCountOf(Ids::WatchId const& watchId) const;

        /**
         * @return The conflicts of every known watch, for the dock tab. Conflicts of a watch not known yet are left
         *         out.
         */
        std::uint64_t conflictCount() const;

        /**
         * @return The failures of every known watch, for the dock tab. Failures of a watch not known yet are left
         *         out.
         */
        std::uint64_t failureCount() const;

        /**
         * @brief Stopped when stop is set; Conflicts when it has conflicts; Failed when it has failures; Syncing when
         *        changes are pending or the entry is uploading; Watching otherwise. Watching for an unknown watch.
         */
        WatchStatus statusOf(Ids::WatchId const& watchId) const;

        /**
         * @brief Stopped for Stopped, NeedsYou for Conflicts and Failed, Syncing for Syncing, Watching otherwise.
         */
        OutlinerGroup groupOf(Ids::WatchId const& watchId) const;

        /**
         * @return The members of @p group that pass @p filter, sorted by name without case, ties by id: watch ids for
         *         the watch groups, instance ids for Orphaned. Orphans whose instance an entry still holds (a stopped
         *         copy) are left out.
         */
        std::vector<Selection> members(OutlinerGroup group, std::string_view filter) const;

        /**
         * @return What the outliner shows beside @p watchId: the conflict count when it has conflicts, else the failed
         *         count when it has failures, else the stop side text when it stopped, else nothing.
         */
        std::string sideTextOf(Ids::WatchId const& watchId, FileTrackingTexts const& texts) const;

        /**
         * @return The text of the status badge of @p watchId: the conflict or failed count, Syncing, Up to date or
         *         Stopped, following @ref statusOf.
         */
        std::string statusTextOf(Ids::WatchId const& watchId, FileTrackingTexts const& texts) const;

        /**
         * @return How far the current burst of @p watchId is, 0 to 1: bytesDone / bytesTotal when bytesTotal > 0, else
         *         changesDone / (changesDone + changesPending), 0 without progress or changes.
         */
        double ringFractionOf(Ids::WatchId const& watchId) const;

        /**
         * @return The conflict with the latest raisedAt among the known watches, ties by the greater conflict id.
         */
        std::optional<SharedData::WatchConflict> newestConflict() const;

      private:
        std::map<Ids::WatchId, SharedData::TrackedEntry> entries_{};
        std::map<Ids::WatchId, SharedData::WatchProgress> progress_{};
        std::map<Ids::ConflictId, SharedData::WatchConflict> conflicts_{};
        std::map<Ids::FailureId, SharedData::UploadFailure> failures_{};
        std::vector<SharedData::OrphanedCopy> orphans_{};
    };
}
