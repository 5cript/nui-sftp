#pragma once

#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <frontend/session_components/file_tracking/problem_list.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <ids/ids.hpp>

#include <cstddef>
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

    using ConflictList = ProblemList<Ids::ConflictId, SharedData::WatchConflict>;
    using FailureList = ProblemList<Ids::FailureId, SharedData::UploadFailure>;

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
         * @brief Adds @p conflict or replaces the one with its id, as the newest of its watch. Kept even while its
         *        watch is not known yet.
         */
        void showConflict(SharedData::WatchConflict const& conflict);

        /**
         * @brief Forgets the conflicts @p conflictIds, they were answered or dropped. Unknown ids are skipped.
         */
        void removeConflicts(std::vector<Ids::ConflictId> const& conflictIds);

        /**
         * @brief Adds @p failure or replaces the one with its id (a repeated failure keeps its id and counts attempts),
         *        as the newest of its watch.
         */
        void showFailure(SharedData::UploadFailure const& failure);

        /**
         * @brief Forgets the failures @p failureIds, they went through, were retried or dismissed. Unknown ids are
         *        skipped.
         */
        void removeFailures(std::vector<Ids::FailureId> const& failureIds);

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
         * @return At most @p count conflicts of @p watchId from rank @p offset on, the last shown first.
         */
        std::vector<SharedData::WatchConflict>
        conflictsOf(Ids::WatchId const& watchId, std::size_t offset, std::size_t count) const;

        /**
         * @return At most @p count failures of @p watchId from rank @p offset on, the last shown first.
         */
        std::vector<SharedData::UploadFailure>
        failuresOf(Ids::WatchId const& watchId, std::size_t offset, std::size_t count) const;

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
         * @return The conflict shown last among the known watches, the first of its watch's conflicts.
         */
        std::optional<SharedData::WatchConflict> newestConflict() const;

      private:
        /**
         * @brief The conflicts or the failures of every watch, the watch of each id, and how many the known watches
         *        have.
         */
        template <typename ListT, typename IdT>
        struct ProblemsByWatch
        {
            std::map<Ids::WatchId, ListT> lists{};
            std::map<IdT, Ids::WatchId> watches{};
            /** @brief The problems of the watches entries_ holds. */
            std::uint64_t knownTotal{0u};
        };

        std::map<Ids::WatchId, SharedData::TrackedEntry> entries_{};
        std::map<Ids::WatchId, SharedData::WatchProgress> progress_{};
        ProblemsByWatch<ConflictList, Ids::ConflictId> conflicts_{};
        ProblemsByWatch<FailureList, Ids::FailureId> failures_{};
        std::vector<SharedData::OrphanedCopy> orphans_{};
        /** @brief Orders every show of a conflict or failure, so the newest of different watches compare. */
        std::uint64_t sequence_{0u};
    };
}
