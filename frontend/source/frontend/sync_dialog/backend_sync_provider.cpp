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
    };
    auto gate = std::make_shared<PhaseGate>();
    gate->onBothListed = std::move(onBothListed);

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
                cb();
            }
        },
        std::move(onDiffProgress)
    );
}

void BackendSyncProvider::recompute(
    SharedData::Sync::DiffOptions options,
    std::function<void(SharedData::Sync::DiffSummary)> onSummary
)
{
    if (!queue_ || closed_)
        return;

    queue_->recomputeSyncDiff(
        sessionId_,
        std::move(options),
        [this, onSummary = std::move(onSummary)](SharedData::Sync::DiffSummary summary) mutable
        {
            generation_ = summary.generation;
            onSummary(std::move(summary));
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
        return;

    const auto expectedGeneration = generation_;
    queue_->loadSyncDiffChildren(
        sessionId_,
        section,
        parentRelKey,
        expectedGeneration,
        // Drop stale responses silently: if the generation moved on before the RPC
        // returned the frontend already has a fresher view, and surfacing a stale
        // result would confuse the tree's keyed merge.
        [this, expectedGeneration, onResolved = std::move(onResolved)](
            std::vector<SharedData::Sync::DiffTreeNode> nodes) mutable
        {
            if (expectedGeneration != generation_)
                return;
            onResolved(std::move(nodes));
        },
        std::move(onRejected)
    );
}

void BackendSyncProvider::buildEnqueuePlan(
    SharedData::Sync::DiffSection section,
    std::vector<std::string> selectedRelKeys,
    std::function<void(std::vector<SharedData::Sync::EnqueuePlanEntry>)> onResolved,
    std::function<void(std::string const&)> onRejected
)
{
    if (!queue_ || closed_)
        return;

    const auto expectedGeneration = generation_;
    queue_->buildSyncEnqueuePlan(
        sessionId_,
        section,
        std::move(selectedRelKeys),
        expectedGeneration,
        [this, expectedGeneration, onResolved = std::move(onResolved)](
            std::vector<SharedData::Sync::EnqueuePlanEntry> plan) mutable
        {
            if (expectedGeneration != generation_)
                return;
            onResolved(std::move(plan));
        },
        std::move(onRejected)
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
