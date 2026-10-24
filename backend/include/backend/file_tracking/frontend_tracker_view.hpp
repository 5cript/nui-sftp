#pragma once

#include <backend/file_tracking/tracker_view.hpp>
#include <ids/ids.hpp>

#include <string>
#include <string_view>

namespace Nui
{
    class RpcHub;
}

namespace FileTracking
{
    /**
     * @brief Shows a Tracker in the file tracking panel of its session, which listens to
     *        @c "FileTracker::<sessionId>::<event>": onEntry (SharedData::TrackedEntry), onEntryRemoved (watch id),
     *        onEntriesEnded (SharedData::EndedWatches), onConflict (SharedData::WatchConflict),
     *        onConflictRemoved (conflict id), onFailure (SharedData::UploadFailure), onFailureRemoved (failure id),
     *        onWatchProgress (SharedData::WatchProgress) and onOrphanedCopies (SharedData::OrphanedCopies).
     */
    class FrontendTrackerView : public TrackerView
    {
      public:
        FrontendTrackerView(Nui::RpcHub& hub, Ids::SessionId sessionId);

        void showEntry(SharedData::TrackedEntry const& entry) override;
        void removeEntry(Ids::WatchId const& watchId) override;
        void showEnded(SharedData::EndedWatches const& ended) override;
        void showConflict(SharedData::WatchConflict const& conflict) override;
        void removeConflict(Ids::ConflictId const& conflictId) override;
        void showFailure(SharedData::UploadFailure const& failure) override;
        void removeFailure(Ids::FailureId const& failureId) override;
        void showWatchProgress(SharedData::WatchProgress const& progress) override;
        void showOrphanedCopies(SharedData::OrphanedCopies const& copies) override;

      private:
        std::string eventName(std::string_view event) const;

      private:
        Nui::RpcHub* hub_;
        Ids::SessionId sessionId_;
    };
}
