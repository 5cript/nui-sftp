#pragma once

#include <frontend/session_components/file_tracking/attention_state.hpp>
#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <frontend/session_components/file_tracking/tracking_items.hpp>
#include <frontend/session_components/file_tracking/tracking_model.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>
#include <ids/ids.hpp>

#include <nui/event_system/observed_value.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief The state of the File Tracking panel of one session: the TrackingModel and its projection onto the
     *        observed items the views read, plus the selection, the detail tab, the filter and the folded groups.
     *        TrackerClient feeds it, the views read it, TrackingActions change it. No setter syncs; the entry point (an
     *        event handler, an RPC reply) syncs once.
     */
    class TrackingStore
    {
      public:
        explicit TrackingStore(FileTrackingTexts texts);
        ROAR_PIMPL_SPECIAL_FUNCTIONS_NO_MOVE(TrackingStore);

        /**
         * @brief Adds or updates the item of @p entry, moving it between groups as its status changes. A new watch that
         *        is not stopped is selected while nothing is.
         */
        void showEntry(SharedData::TrackedEntry const& entry);

        /**
         * @brief Drops the item of @p watchId, selecting nothing when it was selected.
         */
        void removeEntry(Ids::WatchId const& watchId);

        /**
         * @brief Adds or replaces @p conflict in its watch's list and updates the watch's counts, status and group.
         */
        void showConflict(SharedData::WatchConflict const& conflict);

        /**
         * @brief Drops the conflict @p conflictId from its watch's list and updates the watch.
         */
        void removeConflict(Ids::ConflictId const& conflictId);

        /**
         * @brief Adds or replaces @p failure in its watch's list and updates the watch's counts, status and group.
         */
        void showFailure(SharedData::UploadFailure const& failure);

        /**
         * @brief Drops the failure @p failureId from its watch's list and updates the watch.
         */
        void removeFailure(Ids::FailureId const& failureId);

        /**
         * @brief Updates the progress, the bars, the in-flight rows, the ring and the status of the watch @p progress
         *        names.
         */
        void showWatchProgress(SharedData::WatchProgress const& progress);

        /**
         * @brief Replaces the orphan items, selecting nothing when the selected orphan is gone.
         */
        void showOrphanedCopies(SharedData::OrphanedCopies const& copies);

        /**
         * @brief Forgets every item and the selection, the session ended. The filter and the folded groups stay.
         */
        void clear();

        /**
         * @return What the tracker sent, for lookups by the facade (the remote path for Open Again, a copy's name for a
         *         confirmation).
         */
        TrackingModel const& model() const;

        /**
         * @return The translated texts the store formats with, for views that format the same way.
         */
        FileTrackingTexts const& texts() const;

        /**
         * @return The items of @p group that pass the filter, in TrackingModel::members order. For Orphaned the list
         *         stays empty, its items are @ref orphanMembers.
         */
        Nui::Observed<std::vector<std::shared_ptr<WatchItem>>>& membersOf(OutlinerGroup group);

        /**
         * @return The orphan items that pass the filter, in TrackingModel::members order.
         */
        Nui::Observed<std::vector<std::shared_ptr<OrphanItem>>>& orphanMembers();

        /**
         * @return The number of members @p group shows, for its count badge and for hiding it while empty.
         */
        std::shared_ptr<Nui::Observed<std::uint64_t>> countOf(OutlinerGroup group);

        /**
         * @return The item of @p watchId, nullptr when it is not known. Kept while filtered out.
         */
        std::shared_ptr<WatchItem> watchItem(Ids::WatchId const& watchId) const;

        /**
         * @return The item of @p instanceId, nullptr when it is not listed. Kept while filtered out.
         */
        std::shared_ptr<OrphanItem> orphanItem(Ids::InstanceId const& instanceId) const;

        /**
         * @return What the detail pane shows.
         */
        Nui::Observed<Selection>& selection();

        /**
         * @brief Shows @p selection in the detail pane.
         */
        void select(Selection selection);

        /**
         * @brief Moves the selection through the members of the expanded groups in display order; Previous and Next
         *        stop at the ends and start at the first or last member while nothing is selected.
         */
        void moveSelection(SelectionMove move);

        /**
         * @return The tab the watch detail shows, the same for every watch. Starts at Conflicts.
         */
        DetailTab detailTab() const;

        /**
         * @brief Shows @p tab in the watch detail, also updating @ref detailTabKey.
         */
        void setDetailTab(DetailTab tab);

        /**
         * @return The data-tab-key of the detail tab (the DetailTab name), the selected key of the tab strip.
         */
        std::shared_ptr<Nui::Observed<std::string>> detailTabKey();

        /**
         * @return The text of the outliner filter.
         */
        Nui::Observed<std::string>& filter();

        /**
         * @brief Filters the outliner by @p filter, see matchesFilter. Keeps the selection even when filtered out.
         */
        void setFilter(std::string filter);

        /**
         * @return Whether @p group is unfolded.
         */
        bool groupExpanded(OutlinerGroup group) const;

        /**
         * @brief Remembers whether @p group is unfolded, for @ref moveSelection. Every group starts unfolded.
         */
        void setGroupExpanded(OutlinerGroup group, bool expanded);

        /**
         * @brief Selects the watch of the newest conflict and its Conflicts tab.
         * @return Whether there was a conflict to show.
         */
        bool focusNewestConflict();

        /**
         * @brief Calls @p onChanged with the conflict and failure counts of every watch after each change of them, so
         *        the dock tab can follow. Replaces an earlier callback.
         */
        void onAttentionChanged(std::function<void(AttentionCounts const& counts)> onChanged);

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
