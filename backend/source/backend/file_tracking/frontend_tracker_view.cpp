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

    void FrontendTrackerView::showEnded(SharedData::EndedWatches const& ended)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onEntriesEnded"), ended);
    }

    void FrontendTrackerView::showConflict(SharedData::WatchConflict const& conflict)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onConflict"), conflict);
    }

    void FrontendTrackerView::removeConflicts(SharedData::RemovedConflicts const& removed)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onConflictsRemoved"), removed);
    }

    void FrontendTrackerView::showFailure(SharedData::UploadFailure const& failure)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onFailure"), failure);
    }

    void FrontendTrackerView::removeFailures(SharedData::RemovedFailures const& removed)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onFailuresRemoved"), removed);
    }

    void FrontendTrackerView::showWatchProgress(SharedData::WatchProgress const& progress)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onWatchProgress"), progress);
    }

    void FrontendTrackerView::showOrphanedCopies(SharedData::OrphanedCopies const& copies)
    {
        RpcHelper::callRemoteSafely(*hub_, eventName("onOrphanedCopies"), copies);
    }

    std::string FrontendTrackerView::eventName(std::string_view event) const
    {
        return fmt::format("FileTracker::{}::{}", sessionId_.value(), event);
    }
}
