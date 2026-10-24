#include <frontend/session_components/file_tracking/tracker_client.hpp>
#include <frontend/session_components/file_tracking/tracking_store.hpp>

#include <utility>

namespace FileTracking
{
    struct TrackerClient::Implementation
    {
        Implementation(TrackingStore& store, std::function<void(SharedData::EndedWatches const& ended)> onEnded)
            : store{&store}
            , onEnded{std::move(onEnded)}
        {}

        TrackingStore* store;
        std::function<void(SharedData::EndedWatches const& ended)> onEnded;
        Ids::SessionId sessionId{};
    };

    TrackerClient::TrackerClient(
        TrackingStore& store,
        std::function<void(SharedData::EndedWatches const& ended)> onEnded
    )
        : impl_{std::make_unique<Implementation>(store, std::move(onEnded))}
    {}

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_MOVE(TrackerClient);

    void TrackerClient::activate([[maybe_unused]] Ids::SessionId sessionId)
    {}

    void TrackerClient::deactivate()
    {}

    void TrackerClient::open(
        [[maybe_unused]] Paths::RemotePath const& remotePath,
        [[maybe_unused]] std::uint64_t sizeBytes,
        [[maybe_unused]] bool watch,
        [[maybe_unused]] std::function<void(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)>
            onOpened,
        [[maybe_unused]] std::function<void(std::string const& reason)> onRefused
    )
    {}

    void TrackerClient::checkUploadAndWatch(
        [[maybe_unused]] Paths::LocalPath const& item,
        [[maybe_unused]] Paths::RemotePath const& directory,
        [[maybe_unused]] std::function<void(Paths::LocalPath const& canonicalItem)> onWatchable,
        [[maybe_unused]] std::function<void(std::string const& reason)> onRefused
    )
    {}

    void TrackerClient::uploadAndWatch(
        [[maybe_unused]] Paths::LocalPath const& item,
        [[maybe_unused]] Paths::RemotePath const& remotePath,
        [[maybe_unused]] std::function<void(std::string const& reason)> onRefused
    )
    {}

    void TrackerClient::discard(
        [[maybe_unused]] Ids::InstanceId const& instanceId,
        [[maybe_unused]] Paths::LocalPath const& localPath
    )
    {}

    void TrackerClient::stop([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackerClient::setOption(
        [[maybe_unused]] Ids::WatchId const& watchId,
        [[maybe_unused]] SharedData::WatchOption option,
        [[maybe_unused]] bool enabled
    )
    {}

    void TrackerClient::answerConflict(
        [[maybe_unused]] Ids::ConflictId const& conflictId,
        [[maybe_unused]] SharedData::ConflictResolution resolution
    )
    {}

    void TrackerClient::answerAllConflicts(
        [[maybe_unused]] Ids::WatchId const& watchId,
        [[maybe_unused]] SharedData::ConflictResolution resolution
    )
    {}

    void TrackerClient::retryFailure([[maybe_unused]] Ids::FailureId const& failureId)
    {}

    void TrackerClient::dismissFailure([[maybe_unused]] Ids::FailureId const& failureId)
    {}

    void TrackerClient::retryAllFailures([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackerClient::dismissAllFailures([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackerClient::removeStopped([[maybe_unused]] Ids::WatchId const& watchId)
    {}

    void TrackerClient::removeAllStopped()
    {}

    void TrackerClient::listOrphanedCopies()
    {}

    void TrackerClient::removeOrphanedCopy(
        [[maybe_unused]] Ids::InstanceId const& instanceId,
        [[maybe_unused]] std::function<void(std::string const& error)> onFailed
    )
    {}

    void TrackerClient::removeAllOrphanedCopies()
    {}
}
