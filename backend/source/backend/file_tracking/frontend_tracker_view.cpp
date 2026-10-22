#include <backend/file_tracking/frontend_tracker_view.hpp>
#include <backend/rpc_helper.hpp>

#include <fmt/format.h>
#include <nui/rpc.hpp>

#include <utility>

namespace FileTracking
{
    FrontendTrackerView::FrontendTrackerView(Nui::RpcHub& hub, Ids::SessionId sessionId)
        : hub_{&hub}
        , sessionId_{std::move(sessionId)}
    {}

    void FrontendTrackerView::showEntry(SharedData::TrackedEntry const& entry)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onEntry"), entry);
    }

    void FrontendTrackerView::removeEntry(Ids::WatchId const& watchId)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onEntryRemoved"), watchId.value());
    }

    void FrontendTrackerView::ask(SharedData::ServerChangedQuestion const& question)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onQuestion"), question);
    }

    void FrontendTrackerView::showOrphanedCount(std::size_t count)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onOrphanedCount"), count);
    }

    void FrontendTrackerView::showProgress(SharedData::TrackerProgress const& progress)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onProgress"), progress);
    }

    std::string FrontendTrackerView::eventName(std::string_view event) const
    {
        return fmt::format("FileTracker::{}::{}", sessionId_.value(), event);
    }
}
