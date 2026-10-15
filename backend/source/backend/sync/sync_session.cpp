#include <backend/sync/sync_session.hpp>
#include <log/log.hpp>

#include <algorithm>
#include <utility>

using namespace SharedData::Sync;

SyncSession::SyncSession(Options options, boost::asio::any_io_executor executor)
    : options_{std::move(options)}
    , executor_{executor}
    , strand_{boost::asio::make_strand(executor_)}
    , cancelled_{std::make_shared<std::atomic<bool>>(false)}
{}

SyncSession::~SyncSession() = default;

void SyncSession::cancel()
{
    if (cancelled_)
        cancelled_->store(true, std::memory_order_release);
}

bool SyncSession::isCancelled() const
{
    return cancelled_ && cancelled_->load(std::memory_order_acquire);
}

void SyncSession::setLocalTreeInStrand(ScanNode tree)
{
    localTree_ = std::move(tree);
}

void SyncSession::setRemoteTreeInStrand(ScanNode tree)
{
    remoteTree_ = std::move(tree);
}

bool SyncSession::bothTreesReadyInStrand() const
{
    return localTree_.has_value() && remoteTree_.has_value();
}

DiffTreeStore& SyncSession::storeFor(DiffSection section)
{
    switch (section)
    {
        case DiffSection::Upload:
            return uploads_;
        case DiffSection::Download:
            return downloads_;
        case DiffSection::Delete:
            return deletes_;
    }
    return uploads_;
}

DiffTreeStore const& SyncSession::storeFor(DiffSection section) const
{
    return const_cast<SyncSession*>(this)->storeFor(section);
}

ScanTrees SyncSession::scanTrees() const
{
    return ScanTrees{
        .local = localTree_ ? &*localTree_ : nullptr,
        .remote = remoteTree_ ? &*remoteTree_ : nullptr,
    };
}

std::expected<SyncSession::ResolvedPaths, Paths::PathError>
SyncSession::resolve(Paths::RelativePath const& relative) const
{
    return options_.localRoot.join(relative).transform(
        [this, &relative](Paths::LocalPath local)
        {
            return ResolvedPaths{.local = std::move(local), .remote = options_.remoteRoot / relative};
        }
    );
}

DiffSummary SyncSession::recomputeDiffInStrand(
    DiffOptions const& requestedOptions,
    std::function<void(std::uint64_t compared)> onProgress
)
{
    auto options = requestedOptions;
    options.localCaseInsensitive = options_.localFoldsCase;
    options.remoteCaseInsensitive = options_.remoteFoldsCase;
    uploads_.clear(options);
    downloads_.clear(options);
    deletes_.clear(options);
    ++generation_;

    DiffSummary summary{};
    summary.sessionId = options_.sessionId;
    summary.generation = generation_;

    if (!localTree_ || !remoteTree_)
        return summary;

    const std::uint64_t totalCount = localTree_->subtreeFileCount + remoteTree_->subtreeFileCount +
        localTree_->children.size() + remoteTree_->children.size();
    summary.heavyCompare = totalCount > options_.heavyCompareThreshold;

    // A cancel only applies to the walk it interrupted.
    cancelled_->store(false, std::memory_order_release);

    StoreDiffSink sink{uploads_, downloads_, deletes_, cancelled_, std::move(onProgress)};
    diffScanTrees(*localTree_, *remoteTree_, options, sink);

    uploads_.finalize();
    downloads_.finalize();
    deletes_.finalize();

    summary.uploads = uploads_.summary();
    summary.downloads = downloads_.summary();
    summary.deletes = deletes_.summary();
    summary.entriesCompared = sink.lastCompared();
    summary.cancelled = sink.cancelled();
    summary.blockedCount = sink.blockedCount();
    summary.blockedSamples = sink.blockedSamples();
    return summary;
}

DiffSnapshot SyncSession::recomputeSnapshotInStrand(
    DiffOptions const& options,
    LoadedParents const& loadedParents,
    std::function<void(std::uint64_t compared)> onProgress
)
{
    DiffSnapshot snapshot{.summary = recomputeDiffInStrand(options, std::move(onProgress))};
    const auto trees = scanTrees();

    const auto collect = [&trees](DiffTreeStore& store, std::vector<std::string> parents)
    {
        // Parents first, so lazily expanded directories exist before their children are asked for. The
        // order is total so equal results compare equal on the frontend.
        std::sort(
            parents.begin(),
            parents.end(),
            [](auto const& lhs, auto const& rhs)
            {
                return lhs.size() != rhs.size() ? lhs.size() < rhs.size() : lhs < rhs;
            }
        );
        std::vector<SectionChildren> result;
        result.push_back(SectionChildren{.parentRelKey = {}, .children = store.loadChildren({}, trees)});
        for (auto& parent : parents)
        {
            if (parent.empty() || !store.find(parent))
                continue;
            auto children = store.loadChildren(parent, trees);
            result.push_back(SectionChildren{.parentRelKey = std::move(parent), .children = std::move(children)});
        }
        return result;
    };

    snapshot.uploads = collect(uploads_, loadedParents.uploads);
    snapshot.downloads = collect(downloads_, loadedParents.downloads);
    snapshot.deletes = collect(deletes_, loadedParents.deletes);
    return snapshot;
}

std::vector<DiffTreeNode> SyncSession::loadChildrenInStrand(DiffSection section, std::string const& parentRelKey)
{
    return storeFor(section).loadChildren(parentRelKey, scanTrees());
}

std::vector<EnqueuePlanEntry>
SyncSession::buildEnqueuePlanInStrand(DiffSection section, std::unordered_set<std::string> const& selectedRelKeys) const
{
    return storeFor(section).buildPlan(selectedRelKeys, scanTrees());
}

SyncSession::ResolvedPlan
SyncSession::resolvePlanInStrand(DiffSection section, std::unordered_set<std::string> const& selectedRelKeys) const
{
    ResolvedPlan result;
    const auto comparedAt = [this](EnqueuePlanEntry const& entry)
    {
        const bool onLocalSide = entry.action == Action::Download || entry.action == Action::DeleteLocal;
        auto const& tree = onLocalSide ? localTree_ : remoteTree_;
        return tree ? comparedStateAt(*tree, entry.relKey) : SharedData::ComparedState{};
    };
    for (auto& entry : buildEnqueuePlanInStrand(section, selectedRelKeys))
    {
        auto paths = Paths::RelativePath::make(entry.relKey).and_then(
            [this](Paths::RelativePath const& relative)
            {
                return resolve(relative);
            }
        );
        if (!paths)
        {
            Log::warn("SyncSession: '{}' has no path on both sides: {}", entry.relKey, paths.error().toString());
            result.refused.push_back(std::move(entry.relKey));
            continue;
        }
        auto compared = comparedAt(entry);
        result.entries.push_back(
            ResolvedPlanEntry{.entry = std::move(entry), .paths = *std::move(paths), .compared = std::move(compared)}
        );
    }
    return result;
}
