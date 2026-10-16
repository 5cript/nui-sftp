#include <shared_data/sync/case_fold.hpp>
#include <shared_data/sync/diff_tree_store.hpp>
#include <shared_data/sync/enqueue_minimizer.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <string_view>
#include <utility>

namespace SharedData::Sync
{
    namespace
    {
        std::string parentOf(std::string_view relKey)
        {
            const auto slash = relKey.rfind('/');
            return slash == std::string_view::npos ? std::string{} : std::string{relKey.substr(0, slash)};
        }

        std::string basenameOf(std::string_view relKey)
        {
            const auto slash = relKey.rfind('/');
            return std::string{slash == std::string_view::npos ? relKey : relKey.substr(slash + 1)};
        }

        std::string joinRelKey(std::string const& parent, std::string const& name)
        {
            return parent.empty() ? name : parent + "/" + name;
        }

        bool isBulkDirectory(DiffTreeNode const& node)
        {
            if (!node.isDirectory || node.isStructural)
                return false;
            if (node.action == Action::DeleteLocal || node.action == Action::DeleteRemote)
                return true;
            return node.typeConflict || node.hasLocalSide != node.hasRemoteSide;
        }

        DiffTreeNode makeStructuralRow(std::string const& relKey)
        {
            return DiffTreeNode{
                .relKey = relKey,
                .name = basenameOf(relKey),
                .action = Action::Upload,
                .isDirectory = true,
                .hasLocalSide = true,
                .hasRemoteSide = true,
                .isStructural = true,
            };
        }
    }

    ScanNode const* findScanNode(ScanNode const& root, std::string_view relKey)
    {
        auto const* current = &root;
        while (!relKey.empty())
        {
            const auto slash = relKey.find('/');
            const auto segment = relKey.substr(0, slash);
            const auto iter = std::lower_bound(
                current->children.begin(),
                current->children.end(),
                segment,
                [](ScanNode const& node, std::string_view name)
                {
                    return node.name < name;
                }
            );
            if (iter == current->children.end() || iter->name != segment)
                return nullptr;
            current = &*iter;
            relKey = slash == std::string_view::npos ? std::string_view{} : relKey.substr(slash + 1);
        }
        return current;
    }

    ComparedState comparedStateAt(ScanNode const& root, std::string_view relKey)
    {
        auto const* node = findScanNode(root, relKey);
        if (!node)
            return ComparedState{};
        return ComparedState{
            .exists = true,
            .type = node->type,
            .size = node->size,
            .mtime = node->mtime,
            .linkTarget = node->type == FileType::Symlink ? node->linkTarget : std::nullopt,
        };
    }

    bool actionSourceIsLocal(Action action)
    {
        return action == Action::Upload || action == Action::DeleteLocal;
    }

    void DiffTreeStore::clear(DiffOptions options)
    {
        options_ = options;
        nodes_.clear();
        children_.clear();
        order_.clear();
        summary_ = SectionSummary{};
    }

    void DiffTreeStore::add(DiffTreeNode node, std::string const& parentRelKey)
    {
        auto relKey = node.relKey;
        children_[parentRelKey].push_back(relKey);
        order_.push_back(relKey);
        nodes_.insert_or_assign(std::move(relKey), std::move(node));
    }

    void DiffTreeStore::finalize()
    {
        const auto emitted = order_.size();
        for (std::size_t index = 0; index < emitted; ++index)
        {
            auto relKey = parentOf(order_[index]);
            while (!relKey.empty() && !nodes_.contains(relKey))
            {
                auto parent = parentOf(relKey);
                children_[parent].push_back(relKey);
                order_.push_back(relKey);
                nodes_.emplace(relKey, makeStructuralRow(relKey));
                relKey = std::move(parent);
            }
        }

        // Siblings share their parent prefix, so ordering the keys orders the names.
        for (auto& [parent, keys] : children_)
            std::sort(keys.begin(), keys.end());

        const auto fillStructural = [this](auto const& self, std::string const& parentRelKey) -> void
        {
            const auto iter = children_.find(parentRelKey);
            if (iter == children_.end())
                return;
            for (auto const& childKey : iter->second)
            {
                auto& child = nodes_.at(childKey);
                if (!child.isStructural)
                    continue;
                self(self, childKey);
                auto const& grandChildren = children_.at(childKey);
                child.directChildCount = static_cast<std::uint32_t>(grandChildren.size());
                child.descendantItemCount = 0;
                child.descendantByteTotal = 0;
                for (auto const& grandChildKey : grandChildren)
                {
                    auto const& grandChild = nodes_.at(grandChildKey);
                    child.descendantItemCount += itemWeight(grandChild);
                    child.descendantByteTotal += grandChild.descendantByteTotal;
                }
            }
        };
        fillStructural(fillStructural, std::string{});

        summary_ = SectionSummary{};
        if (const auto roots = children_.find(std::string{}); roots != children_.end())
        {
            summary_.rootChildCount = roots->second.size();
            for (auto const& rootKey : roots->second)
            {
                auto const& root = nodes_.at(rootKey);
                summary_.itemCount += itemWeight(root);
                summary_.transferBytes += root.descendantByteTotal;
            }
        }
    }

    DiffTreeNode const* DiffTreeStore::find(std::string const& relKey) const
    {
        const auto iter = nodes_.find(relKey);
        return iter == nodes_.end() ? nullptr : &iter->second;
    }

    std::vector<DiffTreeNode> DiffTreeStore::loadChildren(std::string const& parentRelKey, ScanTrees const& trees)
    {
        const auto toRows = [this](std::vector<std::string> const& keys)
        {
            std::vector<DiffTreeNode> rows;
            rows.reserve(keys.size());
            for (auto const& key : keys)
                rows.push_back(nodes_.at(key));
            return rows;
        };

        if (const auto iter = children_.find(parentRelKey); iter != children_.end())
            return toRows(iter->second);

        const auto nodeIter = nodes_.find(parentRelKey);
        if (!options_.recursive || nodeIter == nodes_.end() || !isBulkDirectory(nodeIter->second))
            return {};
        const auto parent = nodeIter->second;

        const bool sourceIsLocal = actionSourceIsLocal(parent.action);
        auto const* sourceRoot = sourceIsLocal ? trees.local : trees.remote;
        if (!sourceRoot)
            return {};
        auto const* directory = findScanNode(*sourceRoot, parentRelKey);
        if (!directory || directory->type != FileType::Directory)
            return {};

        auto& keys = children_[parentRelKey];
        keys.reserve(directory->children.size());
        for (auto const& child : directory->children)
        {
            if (!isPlannable(child, options_))
                continue;
            auto childKey = joinRelKey(parentRelKey, child.name);
            nodes_.insert_or_assign(childKey, makeOneSidedRow(child, childKey, parent.action, sourceIsLocal));
            order_.push_back(childKey);
            keys.push_back(std::move(childKey));
        }
        return toRows(keys);
    }

    void DiffTreeStore::appendSubtreePlan(
        ScanNode const& directory,
        std::string const& relKey,
        Action action,
        ScanTrees const& trees,
        std::vector<EnqueuePlanEntry>& plan
    ) const
    {
        const auto makeEntry = [&](ScanNode const& node, std::string const& key)
        {
            return EnqueuePlanEntry{
                .relKey = key,
                .action = action,
                .sizeBytes = node.type == FileType::Directory ? 0u : node.size,
                .mtime = node.mtime,
                .mtimeNsec = node.mtimeNsec,
                .isDirectory = node.type == FileType::Directory,
            };
        };

        // Transferred onto a case folding filesystem, names that differ only in case would meet as one file.
        std::unordered_map<std::string, int> foldedCounts;
        const bool foldsCase = (action == Action::Download && options_.localCaseInsensitive) ||
            (action == Action::Upload && options_.remoteCaseInsensitive);
        if (foldsCase)
        {
            for (auto const& child : directory.children)
                ++foldedCounts[caseFoldedName(child.name)];
        }

        bool anyChild = false;
        for (auto const& child : directory.children)
        {
            // Unknown or untransferable entries are never planned, not even inside a transferred directory.
            if (!isPlannable(child, options_))
                continue;
            if (foldsCase && foldedCounts[caseFoldedName(child.name)] > 1)
                continue;
            anyChild = true;
            auto childKey = joinRelKey(relKey, child.name);
            if (child.type == FileType::Directory)
                appendSubtreePlan(child, childKey, action, trees, plan);
            else
                plan.push_back(makeEntry(child, childKey));
        }
        // Files create their parents, so only directories without transferred content need an entry.
        if (!anyChild)
            plan.push_back(makeEntry(directory, relKey));
    }

    void DiffTreeStore::appendDeletePlan(
        ScanNode const& directory,
        std::string const& relKey,
        Action action,
        std::vector<EnqueuePlanEntry>& plan
    ) const
    {
        plan.push_back(
            EnqueuePlanEntry{
                .relKey = relKey,
                .action = action,
                .mtime = directory.mtime,
                .mtimeNsec = directory.mtimeNsec,
                .isDirectory = true,
            }
        );
        for (auto const& child : directory.children)
        {
            if (!isPlannable(child, options_))
                continue;
            auto childKey = joinRelKey(relKey, child.name);
            if (child.type == FileType::Directory)
            {
                appendDeletePlan(child, childKey, action, plan);
                continue;
            }
            plan.push_back(
                EnqueuePlanEntry{
                    .relKey = std::move(childKey),
                    .action = action,
                    .sizeBytes = child.size,
                    .mtime = child.mtime,
                    .mtimeNsec = child.mtimeNsec,
                }
            );
        }
    }

    std::vector<EnqueuePlanEntry>
    DiffTreeStore::buildPlan(std::unordered_set<std::string> const& selectedRelKeys, ScanTrees const& trees) const
    {
        std::vector<MinimizerItemView> views;
        views.reserve(order_.size());
        for (auto const& relKey : order_)
        {
            views.push_back(
                MinimizerItemView{
                    .relKey = relKey,
                    .isBulkDir = isBulkDirectory(nodes_.at(relKey)),
                }
            );
        }

        std::vector<EnqueuePlanEntry> plan;
        for (const auto index : minimizeEnqueueIndices(views, selectedRelKeys))
        {
            auto const& node = nodes_.at(views[index].relKey);
            const bool sourceIsLocal = actionSourceIsLocal(node.action);
            auto const* sourceRoot = sourceIsLocal ? trees.local : trees.remote;
            auto const* scan = sourceRoot ? findScanNode(*sourceRoot, node.relKey) : nullptr;
            const bool isDelete = node.action == Action::DeleteLocal || node.action == Action::DeleteRemote;

            if (scan && node.isDirectory && options_.recursive && scan->type == FileType::Directory)
            {
                if (isDelete)
                    appendDeletePlan(*scan, node.relKey, node.action, plan);
                else
                    appendSubtreePlan(*scan, node.relKey, node.action, trees, plan);
                continue;
            }

            plan.push_back(
                EnqueuePlanEntry{
                    .relKey = node.relKey,
                    .action = node.action,
                    .sizeBytes = node.isDirectory
                        ? 0u
                        : (scan ? scan->size : (sourceIsLocal ? node.localSize : node.remoteSize)),
                    .mtime = scan ? scan->mtime : (sourceIsLocal ? node.localMtime : node.remoteMtime),
                    .mtimeNsec = scan ? scan->mtimeNsec : 0u,
                    .isDirectory = node.isDirectory,
                }
            );
        }
        return plan;
    }

    StoreDiffSink::StoreDiffSink(
        DiffTreeStore& uploads,
        DiffTreeStore& downloads,
        DiffTreeStore& deletes,
        std::shared_ptr<std::atomic<bool>> cancelled,
        std::function<void(std::uint64_t)> onProgress
    )
        : uploads_{&uploads}
        , downloads_{&downloads}
        , deletes_{&deletes}
        , cancelled_{std::move(cancelled)}
        , onProgress_{std::move(onProgress)}
    {}

    void StoreDiffSink::emitUpload(DiffTreeNode node, std::string const& parentRelKey)
    {
        uploads_->add(std::move(node), parentRelKey);
    }

    void StoreDiffSink::emitDownload(DiffTreeNode node, std::string const& parentRelKey)
    {
        downloads_->add(std::move(node), parentRelKey);
    }

    void StoreDiffSink::emitDelete(DiffTreeNode node, std::string const& parentRelKey)
    {
        deletes_->add(std::move(node), parentRelKey);
    }

    void StoreDiffSink::onBlocked(std::string const& relKey, BlockReason reason)
    {
        ++blockedCount_;
        if (blockedSamples_.size() < maxBlockedSamples)
            blockedSamples_.push_back(BlockedEntry{.relKey = relKey, .reason = reason});
    }

    void StoreDiffSink::onProgress(std::uint64_t entriesCompared)
    {
        lastCompared_ = entriesCompared;
        if (onProgress_)
            onProgress_(entriesCompared);
    }

    bool StoreDiffSink::cancelled() const
    {
        return cancelled_ && cancelled_->load(std::memory_order_acquire);
    }
}
