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
     *        onEntriesEnded (SharedData::EndedWatches), onQuestion (SharedData::ServerChangedQuestion),
     *        onOrphanedCount (count) and onProgress (SharedData::TrackerProgress).
     */
    class FrontendTrackerView : public TrackerView
    {
      public:
        FrontendTrackerView(Nui::RpcHub& hub, Ids::SessionId sessionId);

        void showEntry(SharedData::TrackedEntry const& entry) override;
        void removeEntry(Ids::WatchId const& watchId) override;
        void showEnded(SharedData::EndedWatches const& ended) override;
        void ask(SharedData::ServerChangedQuestion const& question) override;
        void showOrphanedCount(std::size_t count) override;
        void showProgress(SharedData::TrackerProgress const& progress) override;

      private:
        std::string eventName(std::string_view event) const;

      private:
        Nui::RpcHub* hub_;
        Ids::SessionId sessionId_;
    };
}
