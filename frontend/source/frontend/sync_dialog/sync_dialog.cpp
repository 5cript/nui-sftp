#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/notifications.hpp>

#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>
#include <log/log.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/api/console.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/style_variant.hpp>
#include <script-nui-components/tree.hpp>

#include <ui5-sap-icons/icons/synchronize.hpp>
#include <ui5-sap-icons/icons/minimize.hpp>
#include <ui5-sap-icons/icons/upload.hpp>
#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/arrow-right.hpp>
#include <ui5-sap-icons/icons/arrow-left.hpp>
#include <ui5-sap-icons/icons/slim-arrow-down.hpp>
#include <ui5-sap-icons/icons/slim-arrow-right.hpp>
#include <ui5-sap-icons/icons/refresh.hpp>
#include <ui5-sap-icons/icons/alert.hpp>
#include <ui5-sap-icons/icons/play.hpp>

#include <frontend/svgs/decline.hpp>

#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/sync_execution.hpp>
#include <shared_data/sync/diff_snapshot.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <iterator>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_literals;

namespace
{
    ScriptNuiComponents::Tree::Texts treeTexts()
    {
        return {
            .loading = language->get("syncDialog", "tree", "loading"),
            .loadFailed = language->get("syncDialog", "tree", "loadFailed"),
            .loadMore = language->get("syncDialog", "tree", "loadMore"),
            .collapseAll = language->get("syncDialog", "tree", "collapseAll"),
            .selectAll = language->get("syncDialog", "tree", "selectAll"),
            .deselectAll = language->get("syncDialog", "tree", "deselectAll"),
        };
    }

    using SyncDirection = SharedData::Sync::Direction;
    using SharedData::Sync::DiffSection;
    using SharedData::Sync::DiffTreeNode;

    std::string formatMtime(std::uint64_t mtime)
    {
        using namespace std::chrono;
        const auto tp = system_clock::time_point{seconds{static_cast<long long>(mtime)}};
        return fmt::format("{:%Y-%m-%d}", floor<days>(tp));
    }

    /** @brief Renders one (local or remote) cell of a diff row from the relevant side
     *         of a @ref DiffTreeNode.  @p hasSide gates visibility; when false the
     *         cell renders as the spacer that keeps the grid columns aligned.
     */
    Nui::ElementRenderer renderCellFromNode(DiffTreeNode const& node, bool isRemote, bool alignRight)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using namespace Nui::Attributes::Literals;
        using Nui::Elements::div;
        using Nui::Elements::span;

        const bool hasSide = isRemote ? node.hasRemoteSide : node.hasLocalSide;
        if (!hasSide)
        {
            return div{class_ = fmt::format("sync-diff-cell empty {}", alignRight ? "sync-diff-cell-right" : "")}();
        }

        const std::uint64_t size = isRemote ? node.remoteSize : node.localSize;
        const std::uint64_t mtime = isRemote ? node.remoteMtime : node.localMtime;
        const std::string sizeStr =
            node.isDirectory ? std::string{} : Utility::formatBytes(static_cast<long long>(size));
        const std::string mtimeStr = mtime > 0 ? formatMtime(mtime) : std::string{};

        return div{
            class_ = fmt::format("sync-diff-cell {}", alignRight ? "sync-diff-cell-right" : ""),
            "title"_attr = node.relKey
        }(
            span{class_ = "name"}(node.name),
            [&]() -> Nui::ElementRenderer
            {
                if (sizeStr.empty())
                    return Nui::nil();
                return span{class_ = "meta"}(sizeStr);
            }(),
            [&]() -> Nui::ElementRenderer
            {
                if (mtimeStr.empty())
                    return Nui::nil();
                return span{class_ = "meta date"}(mtimeStr);
            }()
        );
    }

    DiffTreeNode const* userDataAsDiffTreeNode(std::any const& userData)
    {
        return std::any_cast<DiffTreeNode>(&userData);
    }

    /** @brief Parent relKey (everything before the last '/'), or empty when @p
     *         relKey has no separator.  Used by the sparse-selection helpers.
     */
    std::string sparseParentOf(std::string const& relKey)
    {
        const auto slash = relKey.rfind('/');
        return slash == std::string::npos ? std::string{} : relKey.substr(0, slash);
    }

    /** @brief Walks every parent prefix of @p relKey and returns true if any of
     *         them is present in @p set.  "Effective selection" in the sparse
     *         model: an entry X in the set implies every descendant of X.
     */
    bool sparseAnyAncestorInSet(std::string const& relKey, std::unordered_set<std::string> const& set)
    {
        std::string_view view{relKey};
        while (true)
        {
            const auto slash = view.rfind('/');
            if (slash == std::string_view::npos)
                return false;
            view.remove_suffix(view.size() - slash);
            if (set.contains(std::string{view}))
                return true;
        }
    }

    /** @brief Same walk, but returns the covering ancestor's relKey (or empty
     *         when none is found).  Used for fill-out on uncheck.
     */
    std::string sparseFindCoveringAncestor(std::string const& relKey, std::unordered_set<std::string> const& set)
    {
        std::string_view view{relKey};
        while (true)
        {
            const auto slash = view.rfind('/');
            if (slash == std::string_view::npos)
                return {};
            view.remove_suffix(view.size() - slash);
            std::string candidate{view};
            if (set.contains(candidate))
                return candidate;
        }
    }

    /** @brief True when the row should be anchored on the right column — used by
     *         mid-tree "directory only" rows whose side-placement must match the
     *         leaves underneath.  Upload = left, Download/DeleteRemote = right,
     *         DeleteLocal = left.
     */
    bool actionContentOnRight(SharedData::Sync::Action action)
    {
        switch (action)
        {
            case SharedData::Sync::Action::Upload:
            case SharedData::Sync::Action::DeleteLocal:
                return false;
            case SharedData::Sync::Action::Download:
            case SharedData::Sync::Action::DeleteRemote:
                return true;
        }
        return false;
    }
}

// ---- Implementation ---------------------------------------------------------

struct SyncDialog::Implementation
{
    Nui::Observed<bool> open_{false};
    Nui::Observed<bool> minimized_{false};
    std::filesystem::path localPath_{};
    std::filesystem::path remotePath_{};

    /** @brief Shared with the session, so a minimized dialog keeps its provider when a new flow starts. */
    std::shared_ptr<BackendSyncProvider> provider_{};

    // Settings
    Nui::Observed<std::string> directionStr_{"Both"s};
    SyncDirection direction_{SyncDirection::Both};
    Nui::Observed<bool> respectIgnore_{true};
    Nui::Observed<bool> recursive_{true};
    Nui::Observed<bool> ignoreHidden_{false};
    Nui::Observed<bool> actionUpload_{true};
    Nui::Observed<bool> actionDownload_{true};
    Nui::Observed<bool> actionDelete_{false};

    // What the scans of the current provider ran with; scan switches only apply after a recompare.
    ScanSettings scanned_{};
    Nui::Observed<bool> needsRescan_{false};

    // Backend-authoritative totals for the section headers.
    Nui::Observed<SharedData::Sync::DiffSummary> summary_{};

    // A settings change while a recompute runs queues exactly one follow-up.
    bool recomputeInFlight_{false};
    bool recomputeQueued_{false};

    // Per-row progress, created with the row (or with the plan entry for rows not loaded yet).
    using ProgressObserver = std::shared_ptr<Nui::Observed<double>>;
    std::unordered_map<std::string, ProgressObserver> uploadProgress_{};
    std::unordered_map<std::string, ProgressObserver> downloadProgress_{};
    std::unordered_map<std::string, ProgressObserver> deleteProgress_{};

    // Sparse selection sets: an entry implies its whole subtree. Shared with the Tree via Options::selected.
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> uploadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()
    };
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> downloadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()
    };
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> deleteSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()
    };

    /**
     * @brief Frontend bookkeeping of one section.
     */
    struct SectionState
    {
        /// Every row the tree knows, by relKey.
        std::unordered_map<std::string, DiffTreeNode> rows{};
        /// Bumped on every selection change; the resolver rebuilds its index when it moved.
        std::uint64_t selectionVersion{0};
        std::uint64_t indexedVersion{~std::uint64_t{0}};
        /// Proper ancestors of selected entries, they render as indeterminate.
        std::unordered_set<std::string> ancestorsOfSelected{};
        /// False until the user changes the selection; until then a recompute selects everything.
        bool selectionTouched{false};
        /// What the tree shows, to skip re-rendering a section a recompute left unchanged.
        std::vector<SharedData::Sync::SectionChildren> applied{};
        /** @brief What executions reported per row, see setRowState. */
        std::unordered_map<std::string, double> executionStates{};
        /** @brief Why a failed row failed, for the failures a row explains. */
        std::unordered_map<std::string, std::string> failureReasons{};
    };
    std::array<SectionState, 3> sections_{};

    ScriptNuiComponents::Tree uploadTree_{};
    ScriptNuiComponents::Tree downloadTree_{};
    ScriptNuiComponents::Tree deleteTree_{};

    Nui::Observed<bool> uploadCollapsed_{false};
    Nui::Observed<bool> downloadCollapsed_{false};
    Nui::Observed<bool> deleteCollapsed_{false};

    ConfirmDialog* confirmDialog_;
    OperationQueue* operationQueue_;
    std::function<void(SyncDialog::RecompareRequest)> onRecompareRequested_{};
    /**
     * @brief False while a recompute runs or after one was cancelled: the backend's diff is not the one the rows
     *        show, so Synchronize is off.
     */
    Nui::Observed<bool> diffCurrent_{true};
    /** @brief Operations of executions whose callbacks this dialog registered with the queue. */
    std::vector<Ids::OperationId> registeredOperations_{};
    Utility::LifetimeToken lifetime_{};

    explicit Implementation(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
        : confirmDialog_{confirmDialog}
        , operationQueue_{operationQueue}
    {
        directionStr_ = language->get("syncDialog", "directionBoth");
        initTrees();
    }

    ~Implementation()
    {
        dropRegistrations();
    }
    Implementation(Implementation const&) = delete;
    Implementation& operator=(Implementation const&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;

    SectionState& stateFor(DiffSection section)
    {
        return sections_[static_cast<std::size_t>(section)];
    }

    SectionState const& stateFor(DiffSection section) const
    {
        return sections_[static_cast<std::size_t>(section)];
    }

    std::unordered_map<std::string, ProgressObserver>& progressMapFor(DiffSection section)
    {
        switch (section)
        {
            case DiffSection::Upload:
                return uploadProgress_;
            case DiffSection::Download:
                return downloadProgress_;
            case DiffSection::Delete:
                return deleteProgress_;
        }
        return uploadProgress_;
    }

    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> const& selectionFor(DiffSection section) const
    {
        switch (section)
        {
            case DiffSection::Upload:
                return uploadSelected_;
            case DiffSection::Download:
                return downloadSelected_;
            case DiffSection::Delete:
                return deleteSelected_;
        }
        return uploadSelected_;
    }

    ScriptNuiComponents::Tree& treeFor(DiffSection section)
    {
        switch (section)
        {
            case DiffSection::Upload:
                return uploadTree_;
            case DiffSection::Download:
                return downloadTree_;
            case DiffSection::Delete:
                return deleteTree_;
        }
        return uploadTree_;
    }

    ScriptNuiComponents::Tree::Options treeOptions(DiffSection section, bool mirror)
    {
        return ScriptNuiComponents::Tree::Options{
            .rowContent = makeTreeRowRenderer(section),
            .childrenLoader = makeChildrenLoader(section),
            .showCheckboxes = true,
            .showIcons = false,
            .mirror = mirror,
            .selected = selectionFor(section),
            .selectionStateResolver = makeStateResolver(section),
            .sameUserData =
                [](std::any const& lhs, std::any const& rhs)
            {
                auto const* left = userDataAsDiffTreeNode(lhs);
                auto const* right = userDataAsDiffTreeNode(rhs);
                return left && right && *left == *right;
            },
            .toggleSelection = makeToggleSelection(section),
            .selectAllAction = makeSelectAllAction(section),
            .deselectAllAction = makeDeselectAllAction(section),
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
            .texts = treeTexts(),
        };
    }

    void initTrees()
    {
        uploadTree_ = ScriptNuiComponents::Tree{treeOptions(DiffSection::Upload, false)};
        downloadTree_ = ScriptNuiComponents::Tree{treeOptions(DiffSection::Download, true)};
        deleteTree_ = ScriptNuiComponents::Tree{treeOptions(DiffSection::Delete, false)};
    }

    void selectionChanged(DiffSection section, bool byUser)
    {
        auto& state = stateFor(section);
        ++state.selectionVersion;
        if (byUser)
            state.selectionTouched = true;
    }

    /** @brief Tri-state for sparse selection: self or an ancestor in the set is Checked, an ancestor
     *         of a selected entry is Indeterminate.  O(depth) per row via a lazily rebuilt index.
     */
    std::function<ScriptNuiComponents::Tree::SelectionState(ScriptNuiComponents::Tree::NodeId const&)>
    makeStateResolver(DiffSection section)
    {
        return [this, section](ScriptNuiComponents::Tree::NodeId const& id)
        {
            using State = ScriptNuiComponents::Tree::SelectionState;
            auto const& set = selectionFor(section)->value();
            if (set.contains(id) || sparseAnyAncestorInSet(id, set))
                return State::Checked;

            auto& state = stateFor(section);
            if (state.indexedVersion != state.selectionVersion)
            {
                state.ancestorsOfSelected.clear();
                for (auto const& key : set)
                {
                    for (auto parent = sparseParentOf(key); !parent.empty(); parent = sparseParentOf(parent))
                    {
                        if (!state.ancestorsOfSelected.insert(parent).second)
                            break;
                    }
                }
                state.indexedVersion = state.selectionVersion;
            }
            return state.ancestorsOfSelected.contains(id) ? State::Indeterminate : State::Unchecked;
        };
    }

    std::function<void(ScriptNuiComponents::Tree::NodeId const&, bool, std::unordered_set<std::string>&)>
    makeToggleSelection(DiffSection section)
    {
        return [this, section](
                   ScriptNuiComponents::Tree::NodeId const& id, bool nowSelected, std::unordered_set<std::string>& set
               )
        {
            toggleSparseSelection(section, id, nowSelected, set);
            selectionChanged(section, true);
        };
    }

    std::function<void(std::unordered_set<std::string>&)> makeSelectAllAction(DiffSection section)
    {
        return [this, section](std::unordered_set<std::string>& set)
        {
            set.clear();
            for (auto const& rootId : treeFor(section).childrenOf(std::string{}))
                set.insert(rootId);
            selectionChanged(section, true);
        };
    }

    std::function<void(std::unordered_set<std::string>&)> makeDeselectAllAction(DiffSection section)
    {
        return [this, section](std::unordered_set<std::string>& set)
        {
            set.clear();
            selectionChanged(section, true);
        };
    }

    /** @brief The sparse-toggle core.  On check: drop any descendants already in
     *         the set (they become implied by @p id), insert @p id unless an
     *         ancestor already covers it, then try to collapse up — replace a
     *         full set of siblings at the parent level with the parent itself.
     *
     *         On uncheck: if @p id is in the set, erase it and stop; else walk
     *         up to the covering ancestor, remove it, and "fill out" every
     *         level from the ancestor down to @p id's parent, inserting the
     *         siblings not on the path.  After fill-out @p id and its
     *         descendants are uncovered, every other branch remains covered.
     */
    void toggleSparseSelection(
        DiffSection section,
        std::string const& id,
        bool nowSelected,
        std::unordered_set<std::string>& set
    )
    {
        const auto eraseDescendants = [&set](std::string const& ancestor)
        {
            const std::string prefix = ancestor + "/";
            std::erase_if(
                set,
                [&prefix](std::string const& key)
                {
                    return key.starts_with(prefix);
                }
            );
        };

        if (nowSelected)
        {
            if (sparseAnyAncestorInSet(id, set))
                return;
            eraseDescendants(id);
            set.insert(id);
            collapseUpSparse(section, id, set);
            return;
        }

        if (set.erase(id) > 0)
            return;
        const auto covering = sparseFindCoveringAncestor(id, set);
        if (covering.empty())
        {
            // Checked state came from descendants only.
            eraseDescendants(id);
            return;
        }
        set.erase(covering);
        fillOutSparse(section, covering, id, set);
    }

    /** @brief Walks from @p id up toward the root, replacing fully-selected
     *         sibling groups with their parent.  Stops at the top level —
     *         root-level entries are the sparsest "all selected" representation.
     */
    void collapseUpSparse(DiffSection section, std::string id, std::unordered_set<std::string>& set)
    {
        auto& tree = treeFor(section);
        while (!id.empty())
        {
            const auto parent = sparseParentOf(id);
            if (parent.empty())
                return;
            const auto siblings = tree.childrenOf(parent);
            if (siblings.empty())
                return;
            const bool allIn = std::all_of(
                siblings.begin(),
                siblings.end(),
                [&](auto const& sibling)
                {
                    return set.contains(sibling);
                }
            );
            if (!allIn)
                return;
            for (auto const& sibling : siblings)
                set.erase(sibling);
            set.insert(parent);
            id = parent;
        }
    }

    /** @brief Expands a covering ancestor into per-sibling selections along the
     *         path to @p target.  Precondition: @p ancestor is a strict prefix of
     *         @p target in relKey terms.
     */
    void fillOutSparse(
        DiffSection section,
        std::string const& ancestor,
        std::string const& target,
        std::unordered_set<std::string>& set
    )
    {
        auto& tree = treeFor(section);
        std::string current = ancestor;
        while (current != target)
        {
            const std::string remainder = current.empty() ? target : target.substr(current.size() + 1);
            const auto slash = remainder.find('/');
            const std::string nextSeg = slash == std::string::npos ? remainder : remainder.substr(0, slash);
            const std::string nextChildId = current.empty() ? nextSeg : current + "/" + nextSeg;

            for (auto const& sibling : tree.childrenOf(current))
            {
                if (sibling != nextChildId)
                    set.insert(sibling);
            }
            current = nextChildId;
        }
    }

    /**
     * @brief Remembers rows the tree is about to show and gives file rows their progress observer, which starts
     *        with what an execution already reported for the row.
     */
    void recordRows(DiffSection section, std::vector<DiffTreeNode> const& rows)
    {
        auto& state = stateFor(section);
        auto& progress = progressMapFor(section);
        for (auto const& row : rows)
        {
            state.rows.insert_or_assign(row.relKey, row);
            if (row.isDirectory)
                continue;
            const auto reported = state.executionStates.find(row.relKey);
            progress.try_emplace(
                row.relKey,
                std::make_shared<Nui::Observed<double>>(
                    reported == state.executionStates.end() ? 0.0 : reported->second
                )
            );
        }
    }

    /**
     * @brief Shows the progress of a row: a fraction while it runs, above 1 when done, below 0 when it failed. Only
     *        loaded rows have an observer, the others pick the state up when they load.
     */
    void setRowState(DiffSection section, std::string const& relKey, double value)
    {
        auto& state = stateFor(section);
        state.executionStates.insert_or_assign(relKey, value);
        if (value >= 0.0)
            state.failureReasons.erase(relKey);
        auto const& progress = progressMapFor(section);
        if (const auto observer = progress.find(relKey); observer != progress.end())
            *observer->second = value;
    }

    /**
     * @brief Shows a row as failed with the reason of @p type, for the failures a row explains: a destination or
     *        delete target that changed after the compare. A sync entry may only replace what the compare saw, so a
     *        destination that appeared since fails as existing.
     */
    void setRowFailed(DiffSection section, std::string const& relKey, SharedData::OperationErrorType type)
    {
        using enum SharedData::OperationErrorType;
        if (type == ChangedSinceCompare || type == FileExists)
            stateFor(section).failureReasons.insert_or_assign(
                relKey, std::string{language->get("syncDialog", "changedSinceCompare")}
            );
        setRowState(section, relKey, -1.0);
        // A row shown as failed already only gets its reason.
        auto const& progress = progressMapFor(section);
        if (const auto observer = progress.find(relKey); observer != progress.end())
            observer->second->modify();
    }

    std::string failureReasonOf(DiffSection section, std::string const& relKey) const
    {
        auto const& reasons = stateFor(section).failureReasons;
        const auto found = reasons.find(relKey);
        return found == reasons.end() ? std::string{} : found->second;
    }

    double rowState(DiffSection section, std::string const& relKey) const
    {
        auto const& states = stateFor(section).executionStates;
        const auto found = states.find(relKey);
        return found == states.end() ? 0.0 : found->second;
    }

    ScriptNuiComponents::Tree::ChildrenLoader makeChildrenLoader(DiffSection section)
    {
        return [this, section](
                   ScriptNuiComponents::Tree::NodeId const& parentId,
                   std::function<void(std::vector<ScriptNuiComponents::Tree::Node>)> resolve,
                   std::function<void(std::string)> reject
               )
        {
            if (!provider_)
            {
                reject("no provider");
                return;
            }
            provider_->loadChildren(
                section,
                parentId,
                [this, section, resolve](std::vector<DiffTreeNode> nodes)
                {
                    recordRows(section, nodes);
                    std::vector<ScriptNuiComponents::Tree::Node> result;
                    result.reserve(nodes.size());
                    for (auto& node : nodes)
                        result.push_back(toTreeNode(std::move(node)));
                    resolve(std::move(result));
                },
                [reject](std::string const& msg)
                {
                    reject(msg);
                }
            );
        };
    }

    static ScriptNuiComponents::Tree::Node toTreeNode(DiffTreeNode node)
    {
        const auto id = node.relKey;
        const bool isDir = node.isDirectory;
        return ScriptNuiComponents::Tree::Node{
            .id = id,
            .kind = isDir ? ScriptNuiComponents::Tree::NodeKind::Directory : ScriptNuiComponents::Tree::NodeKind::Leaf,
            .children = {},
            .hasChildren = node.directChildCount > 0,
            .userData = std::move(node),
            .icon = std::nullopt,
            .initiallyExpanded = false,
            .selectable = true,
        };
    }

    ScriptNuiComponents::Tree::RowContentRenderer makeTreeRowRenderer(DiffSection section);

    /** @brief Directories of @p section whose children the tree has loaded, expanded or not. */
    std::vector<std::string> loadedParentsOf(DiffSection section)
    {
        auto& tree = treeFor(section);
        std::vector<std::string> loaded;
        auto pending = tree.childrenOf(std::string{});
        while (!pending.empty())
        {
            auto id = std::move(pending.back());
            pending.pop_back();
            auto children = tree.childrenOf(id);
            if (children.empty())
                continue;
            loaded.push_back(std::move(id));
            pending.insert(
                pending.end(), std::make_move_iterator(children.begin()), std::make_move_iterator(children.end())
            );
        }
        return loaded;
    }

    /** @brief Replaces a section's rows in one go, re-supplying loaded children inline so the tree keeps
     *         expanded directories expanded.  The selection is kept where the user changed it.
     */
    void applySection(DiffSection section, std::vector<SharedData::Sync::SectionChildren> parents)
    {
        auto& state = stateFor(section);
        if (parents == state.applied)
            return;
        state.applied = parents;
        state.rows.clear();

        std::unordered_map<std::string, std::vector<DiffTreeNode>> childrenByParent;
        for (auto& entry : parents)
        {
            recordRows(section, entry.children);
            childrenByParent.insert_or_assign(std::move(entry.parentRelKey), std::move(entry.children));
        }

        const auto build = [&childrenByParent](
                               auto const& self, std::string const& parent
                           ) -> std::vector<ScriptNuiComponents::Tree::Node>
        {
            std::vector<ScriptNuiComponents::Tree::Node> nodes;
            const auto iter = childrenByParent.find(parent);
            if (iter == childrenByParent.end())
                return nodes;
            nodes.reserve(iter->second.size());
            for (auto const& child : iter->second)
            {
                auto node = toTreeNode(child);
                node.children = self(self, child.relKey);
                nodes.push_back(std::move(node));
            }
            return nodes;
        };
        auto& tree = treeFor(section);
        tree.setRoots(build(build, std::string{}));

        // Surviving rows keep their DOM and with it their progress observer.
        std::erase_if(
            progressMapFor(section),
            [&state](auto const& entry)
            {
                return !state.rows.contains(entry.first);
            }
        );
        std::erase_if(
            state.executionStates,
            [&state](auto const& entry)
            {
                return !state.rows.contains(entry.first);
            }
        );
        std::erase_if(
            state.failureReasons,
            [&state](auto const& entry)
            {
                return !state.rows.contains(entry.first);
            }
        );

        auto const& selection = selectionFor(section);
        auto& set = selection->value();
        if (state.selectionTouched)
        {
            std::erase_if(
                set,
                [&state](std::string const& key)
                {
                    return !state.rows.contains(key);
                }
            );
        }
        else
        {
            set.clear();
            for (auto const& rootId : tree.childrenOf(std::string{}))
                set.insert(rootId);
        }
        selectionChanged(section, false);
        selection->modify();
    }

    /**
     * @brief Applies a recompute result to all three sections within one task, so the page lays out and
     *        paints once.  @p collapseZero toggles the collapse state of sections crossing zero rows.
     */
    void applySnapshot(SharedData::Sync::DiffSnapshot snapshot, bool collapseZero)
    {
        auto const& summary = snapshot.summary;
        if (collapseZero)
        {
            const auto prev = summary_.value();
            auto reconcile = [](Nui::Observed<bool>& collapsed, std::uint64_t prevCount, std::uint64_t newCount)
            {
                if (prevCount > 0 && newCount == 0)
                    collapsed = true;
                else if (prevCount == 0 && newCount > 0)
                    collapsed = false;
            };
            reconcile(uploadCollapsed_, prev.uploads.itemCount, summary.uploads.itemCount);
            reconcile(downloadCollapsed_, prev.downloads.itemCount, summary.downloads.itemCount);
            reconcile(deleteCollapsed_, prev.deletes.itemCount, summary.deletes.itemCount);
        }

        summary_ = summary;

        applySection(DiffSection::Upload, std::move(snapshot.uploads));
        applySection(DiffSection::Download, std::move(snapshot.downloads));
        applySection(DiffSection::Delete, std::move(snapshot.deletes));
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    SharedData::Sync::DiffOptions buildDiffOptions() const
    {
        return SharedData::Sync::DiffOptions{
            .direction = direction_,
            .actionUpload = actionUpload_.value(),
            .actionDownload = actionDownload_.value(),
            .actionDelete = actionDelete_.value(),
            .recursive = recursive_.value(),
            .ignoreHidden = ignoreHidden_.value(),
        };
    }

    ScanSettings currentScanSettings() const
    {
        return ScanSettings{
            .respectIgnoreFiles = respectIgnore_.value(),
            .recursive = recursive_.value(),
            .ignoreHidden = ignoreHidden_.value(),
        };
    }

    /** @brief True when the switches ask for data the scans did not collect. */
    void updateNeedsRescan()
    {
        const auto current = currentScanSettings();
        const bool needed = current.respectIgnoreFiles != scanned_.respectIgnoreFiles ||
            (current.recursive && !scanned_.recursive) || (!current.ignoreHidden && scanned_.ignoreHidden);
        if (needsRescan_.value() != needed)
            needsRescan_ = needed;
    }

    /** @brief Runs a backend diff with the current settings.  Changes during a running recompute are
     *         coalesced into one follow-up, and only the newest result is applied.
     */
    void recomputeDiff()
    {
        if (!provider_)
            return;
        if (recomputeInFlight_)
        {
            recomputeQueued_ = true;
            return;
        }
        recomputeInFlight_ = true;
        diffCurrent_ = false;
        provider_->recompute(
            buildDiffOptions(),
            SharedData::Sync::LoadedParents{
                .uploads = loadedParentsOf(DiffSection::Upload),
                .downloads = loadedParentsOf(DiffSection::Download),
                .deletes = loadedParentsOf(DiffSection::Delete),
            },
            lifetime_.guarded(
                [this](SharedData::Sync::DiffSnapshot snapshot)
                {
                    recomputeInFlight_ = false;
                    if (recomputeQueued_)
                    {
                        recomputeQueued_ = false;
                        recomputeDiff();
                        return;
                    }
                    if (snapshot.summary.cancelled)
                    {
                        Nui::globalEventContext.executeActiveEventsImmediately();
                        return;
                    }
                    diffCurrent_ = true;
                    applySnapshot(std::move(snapshot), /*collapseZero=*/true);
                }
            )
        );
    }

    /** @brief Selected items and bytes of one section, from the rows the selection names. */
    std::pair<std::uint64_t, std::uint64_t> selectedTotalsOf(DiffSection section) const
    {
        std::uint64_t items = 0;
        std::uint64_t bytes = 0;
        auto const& rows = stateFor(section).rows;
        for (auto const& key : selectionFor(section)->value())
        {
            const auto iter = rows.find(key);
            if (iter == rows.end())
                continue;
            items += SharedData::Sync::itemWeight(iter->second);
            bytes += iter->second.descendantByteTotal;
        }
        return {items, bytes};
    }

    /** @brief Selected items and bytes over all sections, from the rows the selection names. */
    std::pair<std::uint64_t, std::uint64_t> selectedTotals() const
    {
        std::uint64_t items = 0;
        std::uint64_t bytes = 0;
        for (const auto section : {DiffSection::Upload, DiffSection::Download, DiffSection::Delete})
        {
            const auto [sectionItems, sectionBytes] = selectedTotalsOf(section);
            items += sectionItems;
            bytes += sectionBytes;
        }
        return {items, bytes};
    }

    /** @brief "on the server" or "locally", for the side the delete section removes from. */
    std::string deleteSideText() const
    {
        return language->get("syncDialog", direction_ == SyncDirection::Upload ? "deleteSideRemote" : "deleteSideLocal");
    }

    /** @brief Text of the confirmation before synchronizing: names the deletes and their side. */
    std::string synchronizeConfirmText() const
    {
        auto text = fmt::format(
            fmt::runtime(language->get("syncDialog", "confirmText")),
            localPath_.filename().string(),
            remotePath_.filename().string()
        );
        const auto [deleteItems, deleteBytes] = selectedTotalsOf(DiffSection::Delete);
        if (deleteItems == 0)
            return text;
        return fmt::format(
            "{} {}",
            text,
            fmt::format(
                fmt::runtime(language->get("syncDialog", "confirmDeletes")),
                deleteItems,
                Utility::formatBytes(static_cast<long long>(deleteBytes)),
                deleteSideText()
            )
        );
    }

    /** @brief Runs the row arrow, a delete only after the user confirmed it. */
    void enqueueSingleNodeConfirmed(DiffSection section, std::string const& relKey)
    {
        if (section != DiffSection::Delete)
            return enqueueSingleNode(section, relKey);
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("syncDialog", "confirmDeleteRowHeader"),
            .text = fmt::format(fmt::runtime(language->get("syncDialog", "confirmDeleteRow")), relKey, deleteSideText()),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::No,
            .onClose =
                [this, relKey](std::optional<ConfirmDialog::Button> button)
            {
                if (button == ConfirmDialog::Button::Yes)
                    enqueueSingleNode(DiffSection::Delete, relKey);
            },
        });
    }

    /** @brief Names the skipped entries with their reasons, one per line. */
    std::string blockedSamplesText() const
    {
        auto const& summary = summary_.value();
        const auto sampleLine = [](SharedData::Sync::BlockedEntry const& entry)
        {
            return fmt::format(
                "{} ({})",
                entry.relKey,
                language->get("syncDialog", "blockReason", Utility::enumToString(entry.reason).c_str())
            );
        };
        const auto samples =
            fmt::format("{}", fmt::join(summary.blockedSamples | std::views::transform(sampleLine), "\n"));
        if (summary.blockedCount <= summary.blockedSamples.size())
            return samples;
        return fmt::format(
            "{}\n{}",
            samples,
            fmt::format(
                fmt::runtime(language->get("confirmDialog", "andMore")),
                summary.blockedCount - summary.blockedSamples.size()
            )
        );
    }

    /** @brief Shows the skipped entries in a dialog, its text scrolls however many there are. */
    void showBlockedEntries()
    {
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("syncDialog", "blockedTitle"),
            .text = blockedSamplesText(),
            .buttons = ConfirmDialog::Button::Ok,
            .focusButton = ConfirmDialog::Button::Ok,
        });
    }

    /** @brief The arrow of a row: plans just that row (a directory with its content) and runs it. */
    void enqueueSingleNode(DiffSection section, std::string const& relKey)
    {
        enqueueSelection(section, {relKey}, SharedData::OperationMode::PriorityQueued);
    }

    /**
     * @brief Queues the selection of every section.
     */
    void enqueueOperations()
    {
        if (!provider_ || !diffCurrent_.value())
            return;
        for (const auto section : {DiffSection::Upload, DiffSection::Download, DiffSection::Delete})
        {
            auto const& selected = selectionFor(section)->value();
            if (!selected.empty())
                enqueueSelection(section, {selected.begin(), selected.end()}, SharedData::OperationMode::Queued);
        }
    }

    /**
     * @brief Progress of one operation of a sync execution. Its rows are only known once the backend answers, and
     *        the operation can finish before that, so an early completion or early failures wait here.
     */
    struct SyncPart
    {
        DiffSection section{DiffSection::Upload};
        /** @brief The row of every entry index; a directory delete lists its content as entries of their own. */
        std::vector<std::string> rows{};
        /** @brief Deletes report failures by path, transfers by entry index. */
        std::unordered_map<std::string, std::size_t> indexBySource{};
        std::vector<bool> failed{};
        std::set<std::uint64_t> seenInFlight{};
        bool mapped{false};
        std::optional<bool> completion{};
        std::vector<std::pair<std::string, SharedData::OperationErrorType>> failedSources{};
        std::vector<SharedData::IndexedFailure> failedIndices{};
    };

    /**
     * @brief Shows the entry @p index as failed. Without @p type the reason is not known yet, a later report may
     *        add it.
     */
    void markFailedEntry(SyncPart& part, std::size_t index, std::optional<SharedData::OperationErrorType> type = {})
    {
        if (index >= part.rows.size() || (part.failed[index] && !type))
            return;
        part.failed[index] = true;
        if (type)
            setRowFailed(part.section, part.rows[index], *type);
        else
            setRowState(part.section, part.rows[index], -1.0);
    }

    void markReportedFailures(SyncPart& part)
    {
        for (auto const& [source, type] : std::exchange(part.failedSources, {}))
        {
            if (const auto iter = part.indexBySource.find(source); iter != part.indexBySource.end())
                markFailedEntry(part, iter->second, type);
        }
        for (auto const& failure : std::exchange(part.failedIndices, {}))
            markFailedEntry(part, failure.index, failure.type);
    }

    void completePart(SyncPart& part)
    {
        // Rows already marked failed stay failed on an otherwise successful operation.
        for (std::size_t index = 0; index < part.rows.size(); ++index)
        {
            if (!part.failed[index])
                setRowState(part.section, part.rows[index], *part.completion ? 1.1 : -1.0);
        }
    }

    /** @brief The backend answered with the rows of @p part. */
    void mapPart(SyncPart& part, std::vector<std::string> rows, std::vector<std::string> const& sources)
    {
        part.rows = std::move(rows);
        part.failed.assign(part.rows.size(), false);
        for (std::size_t index = 0; index < sources.size() && index < part.rows.size(); ++index)
            part.indexBySource.emplace(sources[index], index);
        part.mapped = true;
        markReportedFailures(part);
        if (part.completion)
            completePart(part);
    }

    /**
     * @brief Registers the completion of @p operationId with the queue, kept for as long as this dialog shows the
     *        execution, see @ref dropRegistrations.
     */
    void watchCompletion(Ids::OperationId const& operationId, std::shared_ptr<SyncPart> part)
    {
        registeredOperations_.push_back(operationId);
        operationQueue_->addCompletionCallback(
            operationId,
            lifetime_.guarded(
                [this, part](bool success)
                {
                    part->completion = success;
                    if (part->mapped)
                        completePart(*part);
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            )
        );
    }

    /** @brief Files can finish in any order in a bulk, so rows seen in flight are done once they drop out. */
    void watchBulkPart(Ids::OperationId const& operationId, std::shared_ptr<SyncPart> part)
    {
        operationQueue_->addBulkProgressCallback(
            operationId,
            lifetime_.guarded(
                [this, part](SharedData::BulkProgress const& progress)
                {
                    if (!part->mapped)
                        return;
                    for (const auto entryIndex : progress.failedEntryIndices)
                        markFailedEntry(*part, entryIndex);
                    std::set<std::uint64_t> stillInFlight;
                    for (auto const& file : progress.inFlight)
                    {
                        if (file.entryIndex >= part->rows.size() || part->failed[file.entryIndex])
                            continue;
                        stillInFlight.insert(file.entryIndex);
                        part->seenInFlight.insert(file.entryIndex);
                        if (file.totalBytes > 0)
                            setRowState(
                                part->section,
                                part->rows[file.entryIndex],
                                static_cast<double>(file.bytes) / static_cast<double>(file.totalBytes)
                            );
                    }
                    std::erase_if(
                        part->seenInFlight,
                        [&](std::uint64_t entryIndex)
                        {
                            if (stillInFlight.contains(entryIndex))
                                return false;
                            auto const& row = part->rows[entryIndex];
                            if (!part->failed[entryIndex] && rowState(part->section, row) < 1.0)
                                setRowState(part->section, row, 1.1);
                            return true;
                        }
                    );
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            )
        );
        watchFailedEntries(operationId, std::move(part));
    }

    /** @brief Failed entries arrive on completion, by source path and for bulk transfers by index; then completion. */
    void watchFailedEntries(Ids::OperationId const& operationId, std::shared_ptr<SyncPart> part)
    {
        operationQueue_->addFailedEntriesCallback(
            operationId,
            lifetime_.guarded(
                [this, part](
                    std::vector<std::pair<std::string, SharedData::OperationError>> const& failedEntries,
                    std::vector<SharedData::IndexedFailure> const& indexedFailures
                )
                {
                    std::ranges::transform(
                        failedEntries,
                        std::back_inserter(part->failedSources),
                        [](auto const& failure)
                        {
                            return std::pair{failure.first, failure.second.type};
                        }
                    );
                    part->failedIndices.insert(
                        part->failedIndices.end(), indexedFailures.begin(), indexedFailures.end()
                    );
                    if (part->mapped)
                        markReportedFailures(*part);
                }
            )
        );
        watchCompletion(operationId, std::move(part));
    }

    /**
     * @brief Removes the callbacks of every execution this dialog registered with the queue. Their operations keep
     *        running, only the rows they would paint are gone.
     */
    void dropRegistrations()
    {
        if (!operationQueue_)
            return;
        for (auto const& operationId : std::exchange(registeredOperations_, {}))
            operationQueue_->removeCallbacks(operationId);
    }

    /**
     * @brief Has the backend plan and queue the sparse selection @p relKeys. The operation ids are chosen here and
     *        listened to before the request goes out, the answer maps them to rows. A refused request queues
     *        nothing, its rows show as failed and the reason is shown.
     */
    void enqueueSelection(DiffSection section, std::vector<std::string> relKeys, SharedData::OperationMode mode)
    {
        if (!provider_)
            return;

        const SharedData::Sync::SyncExecutionRequest request{
            .section = section,
            .relKeys = std::move(relKeys),
            .mode = mode,
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };
        const auto makePart = [section]()
        {
            return std::make_shared<SyncPart>(SyncPart{.section = section});
        };
        auto uploads = makePart();
        auto downloads = makePart();
        auto remoteDeletes = makePart();
        auto localDeletes = makePart();
        watchBulkPart(request.uploadId, uploads);
        watchBulkPart(request.downloadId, downloads);
        watchFailedEntries(request.remoteDeleteId, remoteDeletes);
        watchFailedEntries(request.localDeleteId, localDeletes);

        provider_->executeSelection(
            request,
            lifetime_.guarded(
                [this, section, request, uploads, downloads, remoteDeletes, localDeletes](
                    SharedData::Sync::SyncExecutionResult result
                )
                {
                    mapPart(*uploads, std::move(result.uploads), {});
                    mapPart(*downloads, std::move(result.downloads), {});
                    mapPart(*remoteDeletes, std::move(result.remoteDeletes), result.remoteDeleteSources);
                    mapPart(*localDeletes, std::move(result.localDeletes), result.localDeleteSources);

                    // A part without rows was never queued and will not complete.
                    for (
                        auto const& [operationId, part] : std::array{
                            std::pair{request.uploadId, uploads},
                            std::pair{request.downloadId, downloads},
                            std::pair{request.remoteDeleteId, remoteDeletes},
                            std::pair{request.localDeleteId, localDeletes},
                        }
                    )
                    {
                        if (part->rows.empty())
                            operationQueue_->removeCallbacks(operationId);
                    }
                    for (auto const& row : result.failed)
                        setRowState(section, row, -1.0);
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            ),
            lifetime_.guarded(
                [this, section, request](std::string const& message)
                {
                    Log::warn("Sync: the backend did not run the selection: {}", message);
                    for (
                        auto const& operationId :
                        {request.uploadId, request.downloadId, request.remoteDeleteId, request.localDeleteId}
                    )
                        operationQueue_->removeCallbacks(operationId);
                    for (auto const& row : request.relKeys)
                        setRowState(section, row, -1.0);
                    Notifications::error(
                        fmt::format(
                            fmt::runtime(language->get("syncDialog", "executionRejected")), language->translate(message)
                        )
                    );
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            )
        );
    }
};

// ---- Tree integration ------------------------------------------------------

ScriptNuiComponents::Tree::RowContentRenderer SyncDialog::Implementation::makeTreeRowRenderer(DiffSection section)
{
    return [this, section](ScriptNuiComponents::Tree::RowContext const& ctx) -> Nui::ElementRenderer
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        DiffTreeNode const* nodePtr = userDataAsDiffTreeNode(ctx.userData);
        const auto folderRow = [](std::string const& name)
        {
            return div{
                class_ = "sync-diff-row sync-diff-row--directory"
            }(span{class_ = "sync-diff-cell sync-diff-cell--directory"}(span{class_ = "name"}(name)),
                div{class_ = "sync-diff-arrow"}(),
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}());
        };
        if (!nodePtr)
        {
            std::string_view view{ctx.id};
            const auto slash = view.rfind('/');
            return folderRow(std::string{slash == std::string_view::npos ? view : view.substr(slash + 1)});
        }

        DiffTreeNode const& node = *nodePtr;

        // Structural rows (synthesized ancestors of deep diffs) have no action of their own.
        if (node.isStructural)
            return folderRow(node.name);

        std::string arrowClass;
        Nui::ElementRenderer arrowIcon = Nui::nil();
        switch (node.action)
        {
            case SharedData::Sync::Action::Upload:
                arrowClass = "upload";
                arrowIcon = Ui5Icons::arrow_right();
                break;
            case SharedData::Sync::Action::Download:
                arrowClass = "download";
                arrowIcon = Ui5Icons::arrow_left();
                break;
            case SharedData::Sync::Action::DeleteLocal:
            case SharedData::Sync::Action::DeleteRemote:
                arrowClass = "delete";
                arrowIcon = Ui5Icons::delete_();
                break;
        }

        const auto relKey = node.relKey;
        auto arrow = div{
            class_ = fmt::format("sync-diff-arrow sync-diff-arrow--clickable {}", arrowClass),
            "title"_attr = language->get("syncDialog", node.typeConflict ? "typeConflictTitle" : "syncItemNowTitle"),
            onClick = [this, section, relKey](Nui::val event)
            {
                event.call<void>("stopPropagation");
                enqueueSingleNodeConfirmed(section, relKey);
            },
        }(std::move(arrowIcon));

        const std::string rowClass = node.typeConflict ? "sync-diff-row sync-diff-row--type-conflict" : "sync-diff-row";

        // Directory rows (one-sided subtree emits): render a folder-style cell on
        // whichever side holds the data.
        if (node.isDirectory)
        {
            const bool labelRight = actionContentOnRight(node.action);
            auto labelCell = span{
                class_ = labelRight ? "sync-diff-cell sync-diff-cell-right sync-diff-cell--directory"
                                    : "sync-diff-cell sync-diff-cell--directory"
            }(span{class_ = "name"}(node.name));
            auto emptyCell = span{class_ = "sync-diff-cell sync-diff-cell--directory"}();
            if (labelRight)
                return div{
                    class_ = rowClass + " sync-diff-row--directory"
                }(std::move(emptyCell), std::move(arrow), std::move(labelCell));
            return div{
                class_ = rowClass + " sync-diff-row--directory"
            }(std::move(labelCell), std::move(arrow), std::move(emptyCell));
        }

        const auto progressIter = progressMapFor(section).find(node.relKey);
        if (progressIter == progressMapFor(section).end())
        {
            return div{
                class_ = rowClass
            }(renderCellFromNode(node, /*isRemote=*/false, /*alignRight=*/false),
                std::move(arrow),
                renderCellFromNode(node, /*isRemote=*/true, /*alignRight=*/true));
        }

        auto progress = progressIter->second;
        return div{
            class_ = rowClass,
            "title"_attr = Nui::observe(*progress).generate(
                [this, section, relKey]()
                {
                    return failureReasonOf(section, relKey);
                }
            ),
            style = Nui::observe(*progress).generate(
                [progress]() -> std::string
                {
                    const double value = progress->value();
                    if (value > 1.0)
                        return "--sync-row-bg: var(--sync-done-color, rgba(76,175,80,0.18));";
                    if (value < 0.0)
                        return "--sync-row-bg: var(--sync-error-color, rgba(231,76,60,0.18));";
                    if (value <= 0.0)
                        return "";
                    const int percent = static_cast<int>(value * 100.0);
                    return fmt::format(
                        "--sync-row-bg: linear-gradient(to right,"
                        " var(--sync-progress-color, rgba(76,175,80,0.25)) {}%,"
                        " transparent {}%);",
                        percent,
                        percent
                    );
                }
            )
        }(renderCellFromNode(node, /*isRemote=*/false, /*alignRight=*/false),
            std::move(arrow),
            renderCellFromNode(node, /*isRemote=*/true, /*alignRight=*/true));
    };
}

// ---- SyncDialog -------------------------------------------------------------

SyncDialog::SyncDialog(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
    : impl_{std::make_unique<Implementation>(confirmDialog, operationQueue)}
{}

SyncDialog::~SyncDialog()
{
    if (moveDetector_.wasMoved())
        return;
}

SyncDialog::SyncDialog(SyncDialog&&) = default;
SyncDialog& SyncDialog::operator=(SyncDialog&&) = default;

void SyncDialog::setOnRecompareRequested(std::function<void(RecompareRequest)> callback)
{
    impl_->onRecompareRequested_ = std::move(callback);
}

SyncDialog::RecompareRequest SyncDialog::currentSettings() const
{
    return RecompareRequest{.scan = impl_->currentScanSettings(), .diffOptions = impl_->buildDiffOptions()};
}

void SyncDialog::open(
    std::shared_ptr<BackendSyncProvider> provider,
    SharedData::Sync::DiffSnapshot snapshot,
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    ScanSettings scanned
)
{
    if (impl_->provider_ != provider)
        impl_->dropRegistrations();
    impl_->provider_ = std::move(provider);
    impl_->diffCurrent_ = true;
    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);
    impl_->scanned_ = scanned;
    impl_->updateNeedsRescan();
    // Replies of the previous provider never arrive, see BackendSyncProvider.
    impl_->recomputeInFlight_ = false;
    impl_->recomputeQueued_ = false;
    for (auto& state : impl_->sections_)
    {
        state.selectionTouched = false;
        state.applied.clear();
    }

    impl_->uploadCollapsed_ = snapshot.summary.uploads.itemCount == 0;
    impl_->downloadCollapsed_ = snapshot.summary.downloads.itemCount == 0;
    impl_->deleteCollapsed_ = snapshot.summary.deletes.itemCount == 0;
    impl_->applySnapshot(std::move(snapshot), /*collapseZero=*/false);

    impl_->open_ = true;
    impl_->minimized_ = false;
    if (impl_->operationQueue_)
        impl_->operationQueue_->hideMinimizedSync();
    Nui::globalEventContext.executeActiveEventsImmediately();
}

Nui::ElementRenderer SyncDialog::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using namespace Nui::Attributes::Literals;
    using Nui::Elements::div;
    using Nui::Elements::span;
    using Nui::Elements::label;
    namespace Snc = ScriptNuiComponents;

    const std::vector<std::string> directionOptions{
        language->get("syncDialog", "directionBoth"),
        language->get("syncDialog", "directionUploadOnly"),
        language->get("syncDialog", "directionDownloadOnly"),
    };
    const std::string directionUploadOnly = directionOptions[1];
    const std::string directionDownloadOnly = directionOptions[2];

    auto onSettingChange = [this]()
    {
        impl_->updateNeedsRescan();
        impl_->recomputeDiff();
        Nui::globalEventContext.executeActiveEventsImmediately();
    };

    // clang-format off
    return div{
        class_ = "sync-dialog-blocker",
        style = observe(impl_->open_, impl_->minimized_).generate([this]() {
            return (impl_->open_.value() && !impl_->minimized_.value())
                ? "display: flex;"s
                : "display: none;"s;
        }),
        onClick = [this](Nui::val event) {
            event.call<void>("stopPropagation");
            impl_->minimized_ = true;
            if (impl_->operationQueue_)
            {
                impl_->operationQueue_->showMinimizedSync([this]() {
                    impl_->minimized_ = false;
                    if (impl_->operationQueue_)
                        impl_->operationQueue_->hideMinimizedSync();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                });
            }
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    }(
        div{
            class_ = "sync-dialog",
            onClick = [](Nui::val event) {
                event.call<void>("stopPropagation");
            }
        }(
            // Header
            div{class_ = "sync-dialog-header"}(
                iconPanel({
                    .icon = Ui5Icons::synchronize(),
                    .color = "var(--theme-color)",
                    .withBorder = true
                }),
                div{class_ = "sync-dialog-title"}(
                    observe(impl_->open_),
                    [this]() -> Nui::ElementRenderer {
                        using Nui::Elements::span;
                        return span{}(fmt::format(
                            fmt::runtime(language->get("syncDialog", "titleFormat")),
                            impl_->localPath_.filename().string(),
                            impl_->remotePath_.filename().string()
                        ));
                    }
                ),
                Snc::button({
                    .icon = Ui5Icons::minimize(),
                    .attributes = {
                        Nui::Attributes::title = language->get("syncDialog", "minimizeTitle"),
                        onClick = [this]() {
                            impl_->minimized_ = true;
                            if (impl_->operationQueue_)
                            {
                                impl_->operationQueue_->showMinimizedSync([this]() {
                                    impl_->minimized_ = false;
                                    if (impl_->operationQueue_)
                                        impl_->operationQueue_->hideMinimizedSync();
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                });
                            }
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        }
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                }),
                Snc::button({
                    .icon = GeneratedSvgs::decline(),
                    .attributes = {
                        Nui::Attributes::title = language->get("syncDialog", "closeTitle"),
                        onClick = [this]() {
                            impl_->open_ = false;
                            impl_->minimized_ = false;
                            // Closing ends the flow: the backend session and the rows' callbacks go.
                            impl_->dropRegistrations();
                            impl_->provider_.reset();
                            if (impl_->operationQueue_)
                                impl_->operationQueue_->hideMinimizedSync();
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        }
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                })
            ),

            // Body
            div{class_ = "sync-dialog-body"}(
                div{class_ = "sync-settings-panel"}(
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}(language->getObserved("syncDialog", "directionLabel")),
                        Snc::select(Snc::SelectOptions<decltype(impl_->directionStr_), std::vector<std::string>>{
                            .activeOption = impl_->directionStr_,
                            .options = directionOptions,
                            .attributes = {style = "min-width: 160px;"},
                            .onChange = [this, onSettingChange, directionUploadOnly, directionDownloadOnly](std::string const& val, Nui::WebApi::MouseEvent const&) {
                                impl_->directionStr_ = val;
                                if (val == directionUploadOnly)
                                    impl_->direction_ = SyncDirection::Upload;
                                else if (val == directionDownloadOnly)
                                    impl_->direction_ = SyncDirection::Download;
                                else
                                    impl_->direction_ = SyncDirection::Both;
                                onSettingChange();
                            }
                        })
                    ),
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}(language->getObserved("syncDialog", "optionsLabel")),
                        div{class_ = "sync-settings-switches"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->recursive_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->recursive_ = val; onSettingChange();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "recursive"))
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->respectIgnore_,
                                    .onChange = [this](bool val, auto const&) {
                                        // Ignore files only filter the scans, the diff has nothing to redo.
                                        impl_->respectIgnore_ = val;
                                        impl_->updateNeedsRescan();
                                        Nui::globalEventContext.executeActiveEventsImmediately();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "respectIgnore"))
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->ignoreHidden_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->ignoreHidden_ = val;
                                        onSettingChange();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "ignoreHidden"))
                            )
                        )
                    ),
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}(language->getObserved("syncDialog", "actionsLabel")),
                        div{class_ = "sync-settings-switches sync-settings-switches-2col"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionUpload_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionUpload_ = val; onSettingChange();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "upload"))
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDownload_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionDownload_ = val; onSettingChange();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "download"))
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDelete_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionDelete_ = val; onSettingChange();
                                    }
                                }),
                                span{}(language->getObserved("syncDialog", "delete"))
                            )
                        )
                    )
                ),

                // Column headers
                div{class_ = "sync-diff-header"}(
                    span{class_ = "sync-diff-col-label"}(language->getObserved("syncDialog", "localHeader")),
                    span{}(),
                    span{class_ = "sync-diff-col-label sync-diff-col-label-right"}(language->getObserved("syncDialog", "remoteHeader")),
                    span{}()
                ),

                // Diff body — three collapsible sections
                div{class_ = "sync-diff-body"}(
                    // Upload
                    div{class_ = "sync-diff-section"}(
                        div{
                            class_ = "sync-diff-section-header",
                            onClick = [this](Nui::val) {
                                impl_->uploadCollapsed_ = !impl_->uploadCollapsed_.value();
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        }(
                            span{class_ = "sync-section-toggle"}(
                                observe(impl_->uploadCollapsed_),
                                [this]() -> Nui::ElementRenderer {
                                    return impl_->uploadCollapsed_.value()
                                        ? Ui5Icons::slim_arrow_right()
                                        : Ui5Icons::slim_arrow_down();
                                }
                            ),
                            Ui5Icons::upload(),
                            span{class_ = "sync-section-label"}(
                                observe(impl_->summary_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    auto const& s = impl_->summary_.value().uploads;
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "uploadSectionCount")),
                                        s.itemCount,
                                        Utility::formatBytes(static_cast<long long>(s.transferBytes))));
                                }
                            )
                        ),
                        div{
                            class_ = "sync-diff-section-rows",
                            style = observe(impl_->uploadCollapsed_).generate([this]() {
                                return impl_->uploadCollapsed_.value() ? "display: none;"s : ""s;
                            })
                        }(
                            impl_->uploadTree_()
                        )
                    ),

                    // Download
                    div{class_ = "sync-diff-section"}(
                        div{
                            class_ = "sync-diff-section-header",
                            onClick = [this](Nui::val) {
                                impl_->downloadCollapsed_ = !impl_->downloadCollapsed_.value();
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        }(
                            span{class_ = "sync-section-toggle"}(
                                observe(impl_->downloadCollapsed_),
                                [this]() -> Nui::ElementRenderer {
                                    return impl_->downloadCollapsed_.value()
                                        ? Ui5Icons::slim_arrow_right()
                                        : Ui5Icons::slim_arrow_down();
                                }
                            ),
                            Ui5Icons::download(),
                            span{class_ = "sync-section-label"}(
                                observe(impl_->summary_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    auto const& s = impl_->summary_.value().downloads;
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "downloadSectionCount")),
                                        s.itemCount,
                                        Utility::formatBytes(static_cast<long long>(s.transferBytes))));
                                }
                            )
                        ),
                        div{
                            class_ = "sync-diff-section-rows",
                            style = observe(impl_->downloadCollapsed_).generate([this]() {
                                return impl_->downloadCollapsed_.value() ? "display: none;"s : ""s;
                            })
                        }(
                            impl_->downloadTree_()
                        )
                    ),

                    // Delete
                    div{class_ = "sync-diff-section"}(
                        div{
                            class_ = "sync-diff-section-header",
                            onClick = [this](Nui::val) {
                                impl_->deleteCollapsed_ = !impl_->deleteCollapsed_.value();
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        }(
                            span{class_ = "sync-section-toggle"}(
                                observe(impl_->deleteCollapsed_),
                                [this]() -> Nui::ElementRenderer {
                                    return impl_->deleteCollapsed_.value()
                                        ? Ui5Icons::slim_arrow_right()
                                        : Ui5Icons::slim_arrow_down();
                                }
                            ),
                            Ui5Icons::delete_(),
                            span{class_ = "sync-section-label"}(
                                observe(impl_->summary_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    auto const& s = impl_->summary_.value().deletes;
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "deleteSectionCount")),
                                        s.itemCount,
                                        Utility::formatBytes(static_cast<long long>(s.transferBytes))));
                                }
                            )
                        ),
                        div{
                            // Delete rows mirror when the direction favors the
                            // remote side (DeleteRemote).  We infer from the
                            // current direction setting — with direction=Upload
                            // deletes happen on remote.
                            class_ = observe(impl_->directionStr_).generate([this]() -> std::string {
                                return impl_->direction_ == SyncDirection::Upload
                                    ? std::string{"sync-diff-section-rows script-nui-tree--mirrored"}
                                    : std::string{"sync-diff-section-rows"};
                            }),
                            style = observe(impl_->deleteCollapsed_).generate([this]() {
                                return impl_->deleteCollapsed_.value() ? "display: none;"s : ""s;
                            })
                        }(
                            impl_->deleteTree_()
                        )
                    )
                )
            ),

            // Entries sync leaves alone, listed in a dialog on click.
            div{}(
                observe(impl_->summary_),
                [this]() -> Nui::ElementRenderer {
                    using Nui::Elements::div;
                    const auto blocked = impl_->summary_.value().blockedCount;
                    if (blocked == 0)
                        return Nui::nil();
                    return div{
                        class_ = "sync-blocked-notice",
                        onClick = [this]() { impl_->showBlockedEntries(); }
                    }(
                        Ui5Icons::alert(),
                        span{}(fmt::format(fmt::runtime(language->get("syncDialog", "blockedNotice")), blocked))
                    );
                }
            ),

            // Footer
            div{class_ = "sync-dialog-footer"}(
                div{class_ = "sync-footer-actions"}(
                    div{
                        class_ = observe(impl_->needsRescan_).generate([this]() {
                            return impl_->needsRescan_.value() ? "sync-recompare sync-recompare--needed"s : "sync-recompare"s;
                        })
                    }(
                        Snc::button({
                            .text = language->getObserved("syncDialog", "recompare"),
                            .icon = Ui5Icons::refresh(),
                            .attributes = {
                                onClick = [this](Nui::val) {
                                    if (impl_->onRecompareRequested_)
                                        impl_->onRecompareRequested_(RecompareRequest{
                                            .scan = impl_->currentScanSettings(),
                                            .diffOptions = impl_->buildDiffOptions(),
                                        });
                                }
                            }
                        }),
                        span{class_ = "sync-recompare-hint"}(language->getObserved("syncDialog", "recompareNeeded"))
                    ),
                    div{class_ = "sync-queue-status"}(
                        observe(impl_->operationQueue_->pausedState()),
                        [this]() -> Nui::ElementRenderer {
                            using Nui::Elements::div;
                            using Nui::Elements::span;
                            if (impl_->operationQueue_->pausedState().value())
                            {
                                return Snc::button({
                                    .text = language->getObserved("syncDialog", "resumeQueue"),
                                    .icon = Ui5Icons::play(),
                                    .attributes = {
                                        onClick = [this](Nui::val) { impl_->operationQueue_->unpause(); }
                                    },
                                    .styleVariant = Snc::StyleVariant::Success,
                                });
                            }
                            return div{class_ = "sync-queue-running-indicator"}(
                                div{class_ = "sync-queue-running-dot"}(),
                                span{}(language->getObserved("syncDialog", "queueRunning"))
                            );
                        }
                    ),
                    div{class_ = "sync-footer-summary"}(
                        observe(
                            *impl_->uploadSelected_, *impl_->downloadSelected_, *impl_->deleteSelected_,
                            impl_->summary_
                        ),
                        [this]() -> Nui::ElementRenderer {
                            using Nui::Elements::span;
                            const auto [items, bytes] = impl_->selectedTotals();
                            return span{}(fmt::format(
                                fmt::runtime(language->get("syncDialog", "footerSummary")),
                                items,
                                Utility::formatBytes(static_cast<long long>(bytes))
                            ));
                        }
                    ),
                    Snc::button({
                        .text = language->getObserved("syncDialog", "synchronize"),
                        .icon = Ui5Icons::synchronize(),
                        .attributes = {
                            disabled = observe(impl_->diffCurrent_).generate([this]() -> std::optional<bool> {
                                return impl_->diffCurrent_.value() ? std::nullopt : std::optional{true};
                            }),
                            Nui::Attributes::title = observe(impl_->diffCurrent_).generate([this]() {
                                return impl_->diffCurrent_.value()
                                    ? std::string{}
                                    : language->get("syncDialog", "synchronizeUnavailable");
                            }),
                            onClick = [this](Nui::val) {
                                if (!impl_->diffCurrent_.value())
                                    return;
                                const bool deletes = impl_->selectedTotalsOf(DiffSection::Delete).first > 0;
                                impl_->confirmDialog_->open({
                                    .styleVariant = deletes ? Snc::StyleVariant::Danger : Snc::StyleVariant::Warning,
                                    .headerText = language->get("syncDialog", "confirmHeader"),
                                    .text = impl_->synchronizeConfirmText(),
                                    .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
                                    .focusButton = deletes ? std::optional{ConfirmDialog::Button::No} : std::nullopt,
                                    .onClose = [this](std::optional<ConfirmDialog::Button> btn) {
                                        if (btn == ConfirmDialog::Button::Yes)
                                            impl_->enqueueOperations();
                                    }
                                });
                            }
                        },
                        .styleVariant = Snc::StyleVariant::Warning
                    })
                )
            )
        )
    );
    // clang-format on
}
