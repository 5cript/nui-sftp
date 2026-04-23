#include <backend/sync/sync_session.hpp>

#include <shared_data/sync/enqueue_minimizer.hpp>

#include <algorithm>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>

using namespace SharedData::Sync;

namespace
{
    /**
     * @brief DiffSink implementation that fans each emission into the matching section
     *        store.  Also feeds the running compared-count up to an external lambda and
     *        consults the session-owned cancel flag.
     *
     * All methods run on the session strand (inherited from the caller of
     * @ref diffScanTrees); no locking needed.
     */
    class SessionDiffSink final : public DiffSink
    {
      public:
        using ProgressFn = std::function<void(std::uint64_t)>;
        using StoreRef = std::unordered_map<std::string, std::vector<DiffTreeNode>>&;
        using NodeMapRef = std::unordered_map<std::string, DiffTreeNode>&;
        using OrderRef = std::vector<std::string>&;

        struct Side
        {
            NodeMapRef nodesByRelKey;
            StoreRef childrenByParent;
            OrderRef emissionOrder;
            SectionSummary& summary;
        };

        SessionDiffSink(
            Side uploads,
            Side downloads,
            Side deletes,
            std::shared_ptr<std::atomic<bool>> cancelled,
            ProgressFn onProgress
        )
            : uploads_{uploads}
            , downloads_{downloads}
            , deletes_{deletes}
            , cancelled_{std::move(cancelled)}
            , onProgress_{std::move(onProgress)}
        {}

        void emitUpload(DiffTreeNode node, std::string const& parentRelKey) override
        {
            emitInto(uploads_, std::move(node), parentRelKey);
        }
        void emitDownload(DiffTreeNode node, std::string const& parentRelKey) override
        {
            emitInto(downloads_, std::move(node), parentRelKey);
        }
        void emitDelete(DiffTreeNode node, std::string const& parentRelKey) override
        {
            emitInto(deletes_, std::move(node), parentRelKey);
        }

        void onProgress(std::uint64_t entriesCompared) override
        {
            lastCompared_ = entriesCompared;
            if (onProgress_)
                onProgress_(entriesCompared);
        }

        bool cancelled() const override
        {
            return cancelled_ && cancelled_->load(std::memory_order_acquire);
        }

        std::uint64_t lastCompared() const
        {
            return lastCompared_;
        }

      private:
        void emitInto(Side& side, DiffTreeNode node, std::string const& parentRelKey)
        {
            side.summary.itemCount += 1;
            if (parentRelKey.empty())
                side.summary.rootChildCount += 1;

            // A leaf contributes its own transferable side; a one-sided directory
            // contributes the pre-computed subtree total (already on the correct side).
            if (node.isDirectory)
                side.summary.transferBytes += node.descendantByteTotal;
            else
                side.summary.transferBytes += node.hasLocalSide ? node.localSize : node.remoteSize;

            const auto leafKey = node.relKey;
            side.emissionOrder.push_back(leafKey);
            side.nodesByRelKey[leafKey] = node;
            side.childrenByParent[parentRelKey].push_back(std::move(node));
        }

        Side uploads_;
        Side downloads_;
        Side deletes_;
        std::shared_ptr<std::atomic<bool>> cancelled_;
        ProgressFn onProgress_;
        std::uint64_t lastCompared_{0};
    };
}

namespace
{
    /** @brief Returns the parent relKey of @p relKey (everything before the last
     *         '/'), or an empty string when @p relKey has no separator.
     */
    std::string parentOf(std::string const& relKey)
    {
        const auto slash = relKey.rfind('/');
        if (slash == std::string::npos)
            return {};
        return relKey.substr(0, slash);
    }

    std::string basenameOf(std::string const& relKey)
    {
        const auto slash = relKey.rfind('/');
        if (slash == std::string::npos)
            return relKey;
        return relKey.substr(slash + 1);
    }

    /** @brief Resolves @p relKey against @p root by walking the sorted children
     *         at each level.  Returns nullptr if any segment can't be matched.
     */
    SharedData::Sync::ScanNode const* findScanNode(
        SharedData::Sync::ScanNode const& root,
        std::string const& relKey
    )
    {
        if (relKey.empty())
            return &root;
        auto const* cur = &root;
        std::size_t start = 0;
        while (start < relKey.size())
        {
            const auto slash = relKey.find('/', start);
            const auto seg = (slash == std::string::npos)
                ? std::string_view{relKey}.substr(start)
                : std::string_view{relKey}.substr(start, slash - start);
            auto iter = std::lower_bound(
                cur->children.begin(),
                cur->children.end(),
                seg,
                [](SharedData::Sync::ScanNode const& n, std::string_view s) { return n.name < s; }
            );
            if (iter == cur->children.end() || iter->name != seg)
                return nullptr;
            cur = &*iter;
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        return cur;
    }

    /** @brief Builds a one-sided child DiffTreeNode from a ScanNode, carrying the
     *         parent's @p action through.
     */
    SharedData::Sync::DiffTreeNode makeOneSidedChildRow(
        SharedData::Sync::ScanNode const& scan,
        std::string const& childRelKey,
        SharedData::Sync::Action action,
        bool isLocalSide
    )
    {
        const bool isDir = scan.type == SharedData::FileType::Directory;
        return SharedData::Sync::DiffTreeNode{
            .relKey = childRelKey,
            .name = scan.name,
            .action = action,
            .isDirectory = isDir,
            .hasLocalSide = isLocalSide,
            .hasRemoteSide = !isLocalSide,
            .localSize = isLocalSide ? scan.size : 0,
            .remoteSize = isLocalSide ? 0 : scan.size,
            .localMtime = isLocalSide ? scan.mtime : 0,
            .remoteMtime = isLocalSide ? 0 : scan.mtime,
            .directChildCount = isDir ? static_cast<std::uint32_t>(scan.children.size()) : 0u,
            .descendantItemCount = isDir ? scan.subtreeFileCount : 1ull,
            .descendantByteTotal = isDir ? scan.subtreeByteTotal : scan.size,
            .isStructural = false,
        };
    }
}

void SyncSession::DiffTreeStore::clear()
{
    nodesByRelKey.clear();
    childrenByParent.clear();
    emissionOrder.clear();
    summary = SectionSummary{};
}

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

DiffSummary SyncSession::recomputeDiffInStrand(
    DiffOptions const& options,
    std::function<void(std::uint64_t compared)> onProgress
)
{
    uploads_.clear();
    downloads_.clear();
    deletes_.clear();
    ++generation_;

    DiffSummary summary{};
    summary.sessionId = options_.sessionId;
    summary.generation = generation_;
    summary.cancelled = false;

    if (!localTree_ || !remoteTree_)
    {
        // Nothing to diff yet; return zeroed summary so callers don't block.
        return summary;
    }

    const std::uint64_t totalCount = localTree_->subtreeFileCount + remoteTree_->subtreeFileCount +
                                     localTree_->children.size() + remoteTree_->children.size();
    summary.heavyCompare = totalCount > options_.heavyCompareThreshold;

    // Reset the cancel flag at the start of each walk so a previous cancel doesn't
    // bleed into the new run.  A fresh cancel during the walk still works because it
    // stores `true` into the same shared atomic.
    cancelled_->store(false, std::memory_order_release);

    SessionDiffSink sink{
        SessionDiffSink::Side{
            uploads_.nodesByRelKey, uploads_.childrenByParent,
            uploads_.emissionOrder, uploads_.summary},
        SessionDiffSink::Side{
            downloads_.nodesByRelKey, downloads_.childrenByParent,
            downloads_.emissionOrder, downloads_.summary},
        SessionDiffSink::Side{
            deletes_.nodesByRelKey, deletes_.childrenByParent,
            deletes_.emissionOrder, deletes_.summary},
        cancelled_,
        std::move(onProgress)
    };

    diffScanTrees(*localTree_, *remoteTree_, options, sink);

    // Synthesize structural ancestor rows for every deep emission so the
    // frontend can build the tree hierarchy above differing leaves.  A diff on
    // 'a/b/c.txt' emits only 'c.txt' at parent 'a/b' — without this pass, the
    // frontend can't represent 'a' or 'a/b'.
    auto synthesizeStructuralAncestors = [](DiffTreeStore& store) {
        // Snapshot the walk-order emissions — we'll be appending to emissionOrder.
        const auto initialEmissions = store.emissionOrder;
        std::unordered_set<std::string> alreadyHave(initialEmissions.begin(), initialEmissions.end());

        std::vector<std::string> newAncestors;
        for (auto const& relKey : initialEmissions)
        {
            auto ancestor = parentOf(relKey);
            while (!ancestor.empty() && !alreadyHave.contains(ancestor))
            {
                alreadyHave.insert(ancestor);
                newAncestors.push_back(ancestor);
                ancestor = parentOf(ancestor);
            }
        }
        // Sort shortest-first so when we build children lists we always have
        // the parent record in place.
        std::sort(newAncestors.begin(), newAncestors.end(), [](auto const& a, auto const& b) {
            return a.size() < b.size();
        });

        for (auto const& relKey : newAncestors)
        {
            DiffTreeNode structural{
                .relKey = relKey,
                .name = basenameOf(relKey),
                .action = Action::Upload,
                .isDirectory = true,
                .hasLocalSide = true,
                .hasRemoteSide = true,
                .localSize = 0,
                .remoteSize = 0,
                .localMtime = 0,
                .remoteMtime = 0,
                .directChildCount = 0,
                .descendantItemCount = 0,
                .descendantByteTotal = 0,
                .isStructural = true,
            };
            store.nodesByRelKey[relKey] = structural;
            store.childrenByParent[parentOf(relKey)].push_back(structural);
            store.emissionOrder.push_back(relKey);
        }

        // Re-sort each affected childrenByParent list by name so synthesized
        // ancestors slot into the right position alongside emitted leaves.
        for (auto& [parent, children] : store.childrenByParent)
        {
            std::sort(children.begin(), children.end(), [](DiffTreeNode const& a, DiffTreeNode const& b) {
                return a.name < b.name;
            });
        }

        // Fill in directChildCount for synthesized rows now that every
        // children list is finalized.
        for (auto const& relKey : newAncestors)
        {
            if (auto iter = store.childrenByParent.find(relKey); iter != store.childrenByParent.end())
            {
                store.nodesByRelKey[relKey].directChildCount =
                    static_cast<std::uint32_t>(iter->second.size());
            }
            // Propagate the updated count into the parent's child list too.
            const auto parent = parentOf(relKey);
            auto parentListIt = store.childrenByParent.find(parent);
            if (parentListIt == store.childrenByParent.end())
                continue;
            for (auto& child : parentListIt->second)
            {
                if (child.relKey == relKey)
                    child.directChildCount = store.nodesByRelKey[relKey].directChildCount;
            }
        }
    };
    synthesizeStructuralAncestors(uploads_);
    synthesizeStructuralAncestors(downloads_);
    synthesizeStructuralAncestors(deletes_);

    summary.uploads = uploads_.summary;
    summary.downloads = downloads_.summary;
    summary.deletes = deletes_.summary;
    summary.entriesCompared = sink.lastCompared();
    summary.cancelled = sink.cancelled();
    return summary;
}

std::vector<DiffTreeNode> SyncSession::loadChildrenInStrand(
    DiffSection section,
    std::string const& parentRelKey
)
{
    auto& store =
        section == DiffSection::Upload ? uploads_ :
        section == DiffSection::Download ? downloads_ :
                                           deletes_;

    if (const auto iter = store.childrenByParent.find(parentRelKey);
        iter != store.childrenByParent.end())
    {
        return iter->second;
    }

    // No cached children — candidate for lazy expansion of a one-sided
    // directory.  Check that parentRelKey names a known one-sided dir node.
    const auto nodeIt = store.nodesByRelKey.find(parentRelKey);
    if (nodeIt == store.nodesByRelKey.end())
        return {};
    auto const& parentNode = nodeIt->second;
    if (!parentNode.isDirectory || parentNode.isStructural)
        return {};
    // Exactly-one-sided is the only case where the diff walk bailed early;
    // two-sided differing directories don't make sense as a single action.
    const bool oneSided = parentNode.hasLocalSide ^ parentNode.hasRemoteSide;
    if (!oneSided)
        return {};

    // Pick the side that has actual ScanNode data and find the subtree.
    auto const& rootOpt = parentNode.hasLocalSide ? localTree_ : remoteTree_;
    if (!rootOpt)
        return {};
    auto const* scanNode = findScanNode(*rootOpt, parentRelKey);
    if (!scanNode || scanNode->type != SharedData::FileType::Directory)
        return {};

    // Emit each child as a one-sided DiffTreeNode with the parent's action.
    std::vector<DiffTreeNode> produced;
    produced.reserve(scanNode->children.size());
    for (auto const& scanChild : scanNode->children)
    {
        const auto childRel = parentRelKey.empty()
            ? scanChild.name
            : parentRelKey + "/" + scanChild.name;
        auto row =
            makeOneSidedChildRow(scanChild, childRel, parentNode.action, parentNode.hasLocalSide);
        store.nodesByRelKey[childRel] = row;
        store.emissionOrder.push_back(childRel);
        produced.push_back(std::move(row));
    }
    store.childrenByParent[parentRelKey] = produced;
    return produced;
}

namespace
{
    bool nodeIsBulkDir(SharedData::Sync::DiffTreeNode const& node)
    {
        if (!node.isDirectory)
            return false;
        switch (node.action)
        {
            case SharedData::Sync::Action::Upload:
                return !node.hasRemoteSide;
            case SharedData::Sync::Action::Download:
                return !node.hasLocalSide;
            case SharedData::Sync::Action::DeleteLocal:
            case SharedData::Sync::Action::DeleteRemote:
                return true;
        }
        return false;
    }

    /**
     * @brief Resolves the local + remote absolute paths that apply to @p node.
     *        For actions that only touch one side, the other path is still set
     *        to the "would-be" destination — keeps the frontend enqueue wiring
     *        uniform.
     */
    std::pair<std::string, std::string> resolveAbsPaths(
        SharedData::Sync::DiffTreeNode const& node,
        std::filesystem::path const& localRoot,
        std::filesystem::path const& remoteRoot
    )
    {
        const auto rel = std::filesystem::path{node.relKey};
        return {(localRoot / rel).generic_string(), (remoteRoot / rel).generic_string()};
    }
}

std::vector<SharedData::Sync::EnqueuePlanEntry> SyncSession::buildEnqueuePlanInStrand(
    SharedData::Sync::DiffSection section,
    std::unordered_set<std::string> const& selectedRelKeys
) const
{
    auto const& store =
        section == DiffSection::Upload ? uploads_ :
        section == DiffSection::Download ? downloads_ :
                                           deletes_;

    std::vector<SharedData::Sync::MinimizerItemView> views;
    views.reserve(store.emissionOrder.size());
    for (auto const& relKey : store.emissionOrder)
    {
        const auto iter = store.nodesByRelKey.find(relKey);
        if (iter == store.nodesByRelKey.end())
            continue;
        views.push_back(SharedData::Sync::MinimizerItemView{
            .relKey = relKey,
            .isBulkDir = nodeIsBulkDir(iter->second),
        });
    }

    const auto keptIndices = SharedData::Sync::minimizeEnqueueIndices(views, selectedRelKeys);

    std::vector<SharedData::Sync::EnqueuePlanEntry> plan;
    plan.reserve(keptIndices.size());
    for (auto idx : keptIndices)
    {
        auto const& relKey = views[idx].relKey;
        const auto nodeIt = store.nodesByRelKey.find(relKey);
        if (nodeIt == store.nodesByRelKey.end())
            continue;
        auto const& node = nodeIt->second;
        auto [localAbs, remoteAbs] = resolveAbsPaths(node, options_.localRoot, options_.remoteRoot);

        const std::uint64_t sizeBytes = node.isDirectory
            ? 0ull
            : (node.hasLocalSide ? node.localSize : node.remoteSize);

        plan.push_back(SharedData::Sync::EnqueuePlanEntry{
            .relKey = relKey,
            .action = node.action,
            .localAbsPath = std::move(localAbs),
            .remoteAbsPath = std::move(remoteAbs),
            .sizeBytes = sizeBytes,
            .mtime = node.hasLocalSide ? node.localMtime : node.remoteMtime,
            .mtimeNsec = 0,
            .isDirectory = node.isDirectory,
        });
    }

    return plan;
}
