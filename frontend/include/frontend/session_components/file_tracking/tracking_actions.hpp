#pragma once

#include <frontend/session_components/file_tracking/panel_state.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>

#include <functional>
#include <string>

namespace FileTracking
{
    /**
     * @brief Every command the views trigger. The facade (FileTrackingPanel) fills each one, mostly by calling
     *        TrackerClient or TrackingStore, and owns this struct for as long as the views exist, so views may capture
     *        its address. Views call them from DOM event handlers, which sync on their own.
     */
    struct TrackingActions
    {
        /** @brief Replace on Server or Keep Server's for one conflict. */
        std::function<void(Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution)>
            answerConflict{};
        /** @brief Replace All on Server or Keep All Server's for the conflicts of a watch. */
        std::function<void(Ids::WatchId const& watchId, SharedData::ConflictResolution resolution)>
            answerAllConflicts{};
        /** @brief Retry on one failed upload. */
        std::function<void(Ids::FailureId const& failureId)> retryFailure{};
        /** @brief The dismiss icon of one failed upload. */
        std::function<void(Ids::FailureId const& failureId)> dismissFailure{};
        /** @brief Retry All for the failures of a watch. */
        std::function<void(Ids::WatchId const& watchId)> retryAllFailures{};
        /** @brief Dismiss All for the failures of a watch. */
        std::function<void(Ids::WatchId const& watchId)> dismissAllFailures{};
        /** @brief A switch of the Options tab changed. */
        std::function<void(Ids::WatchId const& watchId, SharedData::WatchOption option, bool enabled)> setOption{};
        /** @brief Stop on a running watch; it stays listed under Stopped. */
        std::function<void(Ids::WatchId const& watchId)> stop{};
        /** @brief Remove on a stopped watch, or the trash icon of its row. */
        std::function<void(Ids::WatchId const& watchId)> removeStopped{};
        /** @brief The trash icon of the Stopped group. */
        std::function<void()> removeAllStopped{};
        /** @brief Open Again on a stopped copy: opens its server file again and watches it, then removes the old one.
         */
        std::function<void(Ids::WatchId const& watchId)> openAgain{};
        /** @brief Show “Y” on an overlap stop: selects the other watch. */
        std::function<void(Ids::WatchId const& otherWatchId)> showWatch{};
        /** @brief Delete Copy, or the trash icon of an orphan row. The facade asks for confirmation first. */
        std::function<void(Ids::InstanceId const& instanceId)> deleteOrphanedCopy{};
        /** @brief The trash icon of the Orphaned group. The facade asks for confirmation first. */
        std::function<void()> deleteAllOrphanedCopies{};
        /** @brief Open Local Item on a watch, Show Folder on an orphan: opens @p path on this computer. */
        std::function<void(Paths::LocalPath const& path)> openLocally{};
        /** @brief Reveal on Server: shows @p path in the remote side of the file explorer. */
        std::function<void(Paths::RemotePath const& path)> revealOnServer{};
        /** @brief A click or Enter on an outliner row. */
        std::function<void(Selection const& selection)> select{};
        /** @brief Up, Down, Home or End in the outliner. */
        std::function<void(SelectionMove move)> moveSelection{};
        /** @brief A detail tab was chosen. */
        std::function<void(DetailTab tab)> chooseTab{};
        /** @brief The outliner filter text changed. */
        std::function<void(std::string const& filter)> filter{};
        /** @brief An outliner group was folded or unfolded. */
        std::function<void(OutlinerGroup group, bool expanded)> setGroupExpanded{};
    };
}
