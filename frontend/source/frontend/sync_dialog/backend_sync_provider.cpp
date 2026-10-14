#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <frontend/session_components/operation_queue.hpp>

#include <log/log.hpp>

#include <utility>

BackendSyncProvider::BackendSyncProvider(OperationQueue* queue)
    : queue_{queue}
    , sessionId_{Ids::generateSyncSessionId()}
{}

BackendSyncProvider::~BackendSyncProvider()
{
    close();
}

void BackendSyncProvider::open(
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    std::function<void(SharedData::ScanProgress const&)> onLocalListing,
    std::function<void(SharedData::ScanProgress const&)> onRemoteListing,
    std::function<void()> onBothListed,
    std::function<void(std::string const&)> onScanFailed,
    std::function<void(std::uint64_t)> onDiffProgress
)
{
    if (!queue_)
    {
        Log::error("BackendSyncProvider::open called with no OperationQueue");
        return;
    }

    // Shared state for the two-phase-done gate.  Both sides must finish before we
    // unblock the caller's onBothListed.
    struct PhaseGate
    {
        bool local{false};
        bool remote{false};
        std::function<void()> onBothListed;
        std::function<void(std::string const&)> onScanFailed;
    };
    auto gate = std::make_shared<PhaseGate>();
    gate->onBothListed = std::move(onBothListed);
    gate->onScanFailed = std::move(onScanFailed);

    queue_->openSyncSession(
        sessionId_,
        std::move(localPath),
        std::move(remotePath),
        respectIgnoreFiles,
        recursive,
        ignoreHidden,
        std::move(onRemoteListing),
        std::move(onLocalListing),
        [gate](bool isLocal)
        {
            if (isLocal)
                gate->local = true;
            else
                gate->remote = true;
            if (gate->local && gate->remote && gate->onBothListed)
            {
                auto cb = std::move(gate->onBothListed);
                gate->onBothListed = nullptr;
                gate->onScanFailed = nullptr;
                cb();
            }
        },
        [gate](std::string const& reason)
        {
            // The first failure ends the listing, the other side's result is of no use any more.
            gate->onBothListed = nullptr;
            if (auto callback = std::exchange(gate->onScanFailed, nullptr); callback)
                callback(reason);
        },
        std::move(onDiffProgress)
    );
}

void BackendSyncProvider::recompute(
    SharedData::Sync::DiffOptions options,
    SharedData::Sync::LoadedParents loadedParents,
    std::function<void(SharedData::Sync::DiffSnapshot)> onSnapshot
)
{
    if (!queue_ || closed_)
    {
        SharedData::Sync::DiffSnapshot nothing{};
        nothing.summary.cancelled = true;
        onSnapshot(std::move(nothing));
        return;
    }

    queue_->recomputeSyncDiff(
        sessionId_,
        std::move(options),
        std::move(loadedParents),
        [alive = std::weak_ptr{alive_},
            onSnapshot = std::move(onSnapshot)](SharedData::Sync::DiffSnapshot snapshot) mutable
        {
            const auto self = alive.lock();
            if (!self)
                return;
            if (!snapshot.summary.cancelled)
                (*self)->generation_ = snapshot.summary.generation;
            onSnapshot(std::move(snapshot));
        }
    );
}

void BackendSyncProvider::loadChildren(
    SharedData::Sync::DiffSection section,
    std::string const& parentRelKey,
    std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    if (!queue_ || closed_)
        return onRejected("closed");

    const auto expectedGeneration = generation_;
    queue_->loadSyncDiffChildren(
        sessionId_,
        section,
        parentRelKey,
        expectedGeneration,
        // Drop stale responses silently: if the generation moved on before the RPC
        // returned the frontend already has a fresher view, and surfacing a stale
        // result would confuse the tree's keyed merge.
        [alive = std::weak_ptr{alive_},
            expectedGeneration,
            onResolved = std::move(onResolved)](std::vector<SharedData::Sync::DiffTreeNode> nodes) mutable
        {
            const auto self = alive.lock();
            if (!self || expectedGeneration != (*self)->generation_)
                return;
            onResolved(std::move(nodes));
        },
        [alive = std::weak_ptr{alive_}, onRejected = std::move(onRejected)](std::string const& message)
        {
            if (alive.lock())
                onRejected(message);
        }
    );
}

void BackendSyncProvider::executeSelection(
    SharedData::Sync::SyncExecutionRequest request,
    std::function<void(SharedData::Sync::SyncExecutionResult)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    if (!queue_ || closed_)
        return onRejected("closed");

    request.syncSessionId = sessionId_;
    request.generation = generation_;
    queue_->executeSyncSelection(
        std::move(request),
        [alive = std::weak_ptr{alive_}, onResolved = std::move(onResolved)](SharedData::Sync::SyncExecutionResult result)
        {
            // The operations are queued either way, the reply only maps them to rows of a dialog that may be gone.
            if (alive.lock())
                onResolved(std::move(result));
        },
        [alive = std::weak_ptr{alive_}, onRejected = std::move(onRejected)](std::string const& message)
        {
            if (alive.lock())
                onRejected(message);
        }
    );
}

void BackendSyncProvider::cancelDiff()
{
    if (!queue_ || closed_)
        return;
    queue_->cancelSyncDiff(sessionId_);
}

void BackendSyncProvider::close()
{
    if (closed_ || !queue_)
        return;
    closed_ = true;
    queue_->clearSyncSessionRouting(sessionId_);
    queue_->closeSyncSession(sessionId_);
}
