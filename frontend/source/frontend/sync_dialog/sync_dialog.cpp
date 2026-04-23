#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>

#include <utility/language.hpp>
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
#include <ui5-sap-icons/icons/play.hpp>

#include <frontend/svgs/decline.hpp>

#include <shared_data/file_operations/bulk_add_request.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/enqueue_plan_entry.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <algorithm>
#include <any>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::string_literals;

namespace
{
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
    Nui::ElementRenderer
    renderCellFromNode(DiffTreeNode const& node, bool isRemote, bool alignRight)
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
            [&]() -> Nui::ElementRenderer {
                if (sizeStr.empty())
                    return Nui::nil();
                return span{class_ = "meta"}(sizeStr);
            }(),
            [&]() -> Nui::ElementRenderer {
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
    bool sparseAnyAncestorInSet(
        std::string const& relKey,
        std::unordered_set<std::string> const& set
    )
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
    std::string sparseFindCoveringAncestor(
        std::string const& relKey,
        std::unordered_set<std::string> const& set
    )
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

    /** @brief True when @p set contains any entry strictly below @p relKey.
     *         Linear in |set|.  Used only to flip an Unchecked directory row
     *         into Indeterminate when a descendant was selected individually.
     */
    bool sparseAnyDescendantInSet(
        std::string const& relKey,
        std::unordered_set<std::string> const& set
    )
    {
        const std::string prefix = relKey + "/";
        for (auto const& key : set)
        {
            if (key.size() > prefix.size() && key.starts_with(prefix))
                return true;
        }
        return false;
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

    BackendSyncProvider* provider_{nullptr};

    // Settings
    Nui::Observed<std::string> directionStr_{"Both"s};
    SyncDirection direction_{SyncDirection::Both};
    Nui::Observed<bool> respectIgnore_{true};
    Nui::Observed<bool> recursive_{true};
    Nui::Observed<bool> ignoreHidden_{false};
    Nui::Observed<bool> actionUpload_{true};
    Nui::Observed<bool> actionDownload_{true};
    Nui::Observed<bool> actionDelete_{false};

    // Summary — drives footer totals and section counts.  Backend-authoritative;
    // the frontend never needs the full entry list to compute these.
    Nui::Observed<SharedData::Sync::DiffSummary> summary_{};

    // Per-section progress observers keyed by relKey.  Allocated lazily when a row
    // is enqueued (arrow click or bulk synchronize); the row renderer reads the
    // relKey's observer back to paint the gradient.  When a bulk directory is
    // enqueued, every descendant relKey gets the *same* observer so rows inside
    // the subtree share the parent's progress.
    using ProgressObserver = std::shared_ptr<Nui::Observed<double>>;
    std::unordered_map<std::string, ProgressObserver> uploadProgress_{};
    std::unordered_map<std::string, ProgressObserver> downloadProgress_{};
    std::unordered_map<std::string, ProgressObserver> deleteProgress_{};

    // Drives the row renderer's observe() so the gradient repaints when any
    // progress observer updates.  Bumped after every provider response.  Cheap:
    // one extra int per paint.
    Nui::Observed<int> progressEpoch_{0};

    // Selection sets (leaf relKeys only — directories derive their tristate from
    // descendants).  Shared with the Tree via Options::selected so the tree
    // toolbar buttons manipulate the same storage.
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> uploadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> downloadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> deleteSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};

    ScriptNuiComponents::Tree uploadTree_{};
    ScriptNuiComponents::Tree downloadTree_{};
    ScriptNuiComponents::Tree deleteTree_{};

    Nui::Observed<bool> uploadCollapsed_{false};
    Nui::Observed<bool> downloadCollapsed_{false};
    Nui::Observed<bool> deleteCollapsed_{false};

    ConfirmDialog* confirmDialog_;
    OperationQueue* operationQueue_;
    std::function<void(SyncDialog::RecompareRequest)> onRecompareRequested_{};

    explicit Implementation(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
        : confirmDialog_{confirmDialog}
        , operationQueue_{operationQueue}
    {
        directionStr_ = language->get("syncDialog", "directionBoth");
        initTrees();
    }

    /** @brief Direction bit — "right" means the download/remote-leaning cell. */
    static DiffSection const& sectionForTree(bool isUpload, bool isDownload)
    {
        static const DiffSection upload = DiffSection::Upload;
        static const DiffSection download = DiffSection::Download;
        static const DiffSection del = DiffSection::Delete;
        if (isUpload) return upload;
        if (isDownload) return download;
        return del;
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

    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>>& selectionFor(DiffSection section)
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

    void initTrees()
    {
        namespace Snc = ScriptNuiComponents;
        uploadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(DiffSection::Upload),
            .childrenLoader = makeChildrenLoader(DiffSection::Upload),
            .showCheckboxes = true,
            .showIcons = false,
            .selected = uploadSelected_,
            .selectionStateResolver = makeStateResolver(DiffSection::Upload),
            .toggleSelection = makeToggleSelection(DiffSection::Upload),
            .selectAllAction = makeSelectAllAction(DiffSection::Upload),
            .deselectAllAction = makeDeselectAllAction(),
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
        downloadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(DiffSection::Download),
            .childrenLoader = makeChildrenLoader(DiffSection::Download),
            .showCheckboxes = true,
            .showIcons = false,
            .mirror = true,
            .selected = downloadSelected_,
            .selectionStateResolver = makeStateResolver(DiffSection::Download),
            .toggleSelection = makeToggleSelection(DiffSection::Download),
            .selectAllAction = makeSelectAllAction(DiffSection::Download),
            .deselectAllAction = makeDeselectAllAction(),
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
        deleteTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(DiffSection::Delete),
            .childrenLoader = makeChildrenLoader(DiffSection::Delete),
            .showCheckboxes = true,
            .showIcons = false,
            .selected = deleteSelected_,
            .selectionStateResolver = makeStateResolver(DiffSection::Delete),
            .toggleSelection = makeToggleSelection(DiffSection::Delete),
            .selectAllAction = makeSelectAllAction(DiffSection::Delete),
            .deselectAllAction = makeDeselectAllAction(),
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
    }

    /** @brief Builds the tri-state resolver for sparse semantics: self-in-set
     *         OR ancestor-in-set ⇒ Checked; self not covered but descendant in
     *         set ⇒ Indeterminate; otherwise Unchecked.  Cheap per call — one
     *         ancestor walk up to the root plus at most one linear scan of the
     *         (sparse) set.
     */
    std::function<ScriptNuiComponents::Tree::SelectionState(ScriptNuiComponents::Tree::NodeId const&)>
    makeStateResolver(DiffSection section)
    {
        return [this, section](ScriptNuiComponents::Tree::NodeId const& id) {
            using State = ScriptNuiComponents::Tree::SelectionState;
            auto const& set = selectionFor(section)->value();
            if (set.contains(id))
                return State::Checked;
            if (sparseAnyAncestorInSet(id, set))
                return State::Checked;
            if (sparseAnyDescendantInSet(id, set))
                return State::Indeterminate;
            return State::Unchecked;
        };
    }

    /** @brief Builds the sparse toggle callback.  See the design note in
     *         @c toggleSparseSelection for the algorithm.
     */
    std::function<void(
        ScriptNuiComponents::Tree::NodeId const&,
        bool,
        std::unordered_set<std::string>&)>
    makeToggleSelection(DiffSection section)
    {
        return [this, section](
                   ScriptNuiComponents::Tree::NodeId const& id,
                   bool nowSelected,
                   std::unordered_set<std::string>& set) {
            toggleSparseSelection(section, id, nowSelected, set);
        };
    }

    std::function<void(std::unordered_set<std::string>&)>
    makeSelectAllAction(DiffSection section)
    {
        return [this, section](std::unordered_set<std::string>& set) {
            set.clear();
            auto const roots = treeFor(section).childrenOf(std::string{});
            for (auto const& rootId : roots)
                set.insert(rootId);
        };
    }

    std::function<void(std::unordered_set<std::string>&)> makeDeselectAllAction()
    {
        return [](std::unordered_set<std::string>& set) {
            set.clear();
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
        if (nowSelected)
        {
            if (sparseAnyAncestorInSet(id, set))
                return;  // already effectively selected
            const std::string prefix = id + "/";
            std::vector<std::string> toRemove;
            for (auto const& key : set)
            {
                if (key.size() > prefix.size() && key.starts_with(prefix))
                    toRemove.push_back(key);
            }
            for (auto const& key : toRemove)
                set.erase(key);
            set.insert(id);
            collapseUpSparse(section, id, set);
            return;
        }

        // Uncheck.
        if (set.contains(id))
        {
            set.erase(id);
            return;
        }
        const auto covering = sparseFindCoveringAncestor(id, set);
        if (covering.empty())
        {
            // No ancestor in set — any "checked" state here was contributed by
            // descendants.  Remove them all so the row flips to Unchecked.
            const std::string prefix = id + "/";
            std::vector<std::string> toRemove;
            for (auto const& key : set)
            {
                if (key.size() > prefix.size() && key.starts_with(prefix))
                    toRemove.push_back(key);
            }
            for (auto const& key : toRemove)
                set.erase(key);
            return;
        }
        set.erase(covering);
        fillOutSparse(section, covering, id, set);
    }

    /** @brief Walks from @p id up toward the root, replacing fully-selected
     *         sibling groups with their parent.  Stops at the top level —
     *         root-level entries are the sparsest "all selected" representation.
     */
    void collapseUpSparse(
        DiffSection section,
        std::string id,
        std::unordered_set<std::string>& set
    )
    {
        auto& tree = treeFor(section);
        while (!id.empty())
        {
            const auto parent = sparseParentOf(id);
            if (parent.empty())
                return;  // don't collapse top-level into synthetic root
            const auto siblings = tree.childrenOf(parent);
            if (siblings.empty())
                return;
            const bool allIn =
                std::all_of(siblings.begin(), siblings.end(), [&](auto const& s) {
                    return set.contains(s);
                });
            if (!allIn)
                return;
            for (auto const& s : siblings)
                set.erase(s);
            set.insert(parent);
            id = parent;
        }
    }

    /** @brief Expands a covering ancestor into per-sibling selections along the
     *         path to @p target.  At every level between @p ancestor and @p
     *         target, inserts every sibling NOT on the path; descends into the
     *         path child.  Precondition: @p ancestor is a strict prefix of
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
            const std::string remainder =
                current.empty() ? target : target.substr(current.size() + 1);
            const auto slash = remainder.find('/');
            const std::string nextSeg =
                slash == std::string::npos ? remainder : remainder.substr(0, slash);
            const std::string nextChildId =
                current.empty() ? nextSeg : current + "/" + nextSeg;

            const auto siblings = tree.childrenOf(current);
            for (auto const& sib : siblings)
            {
                if (sib != nextChildId)
                    set.insert(sib);
            }
            current = nextChildId;
        }
    }

    ScriptNuiComponents::Tree::ChildrenLoader makeChildrenLoader(DiffSection section)
    {
        return [this, section](
                   ScriptNuiComponents::Tree::NodeId const& parentId,
                   std::function<void(std::vector<ScriptNuiComponents::Tree::Node>)> resolve,
                   std::function<void(std::string)> reject) {
            if (!provider_)
            {
                reject("no provider");
                return;
            }
            provider_->loadChildren(
                section,
                parentId,
                [resolve](std::vector<DiffTreeNode> nodes) {
                    std::vector<ScriptNuiComponents::Tree::Node> result;
                    result.reserve(nodes.size());
                    for (auto& node : nodes)
                        result.push_back(toTreeNode(std::move(node)));
                    resolve(std::move(result));
                },
                [reject](std::string const& msg) {
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
            .kind = isDir ? ScriptNuiComponents::Tree::NodeKind::Directory
                          : ScriptNuiComponents::Tree::NodeKind::Leaf,
            .children = {},
            .hasChildren = node.directChildCount > 0,
            .userData = std::move(node),
            .icon = std::nullopt,
            .initiallyExpanded = false,
            .selectable = true,
        };
    }

    /** @brief Build the row renderer for one section.  Reads the DiffTreeNode out
     *         of the tree's userData and paints the per-row gradient using an
     *         on-demand progress observer (see @ref progressMapFor).
     */
    ScriptNuiComponents::Tree::RowContentRenderer makeTreeRowRenderer(DiffSection section);

    ScriptNuiComponents::Tree::RowAttributeProvider makeTreeRowAttributes()
    {
        return {};
    }

    /**
     * @brief Applies the new diff summary and (re)seeds the three trees.  The
     *        @p collapseZero flag controls whether sections whose counts cross
     *        the 0-boundary auto-toggle their collapse state (only on initial
     *        open / on settings-triggered recompute).
     */
    void applySummaryAndReseed(SharedData::Sync::DiffSummary summary, bool collapseZero)
    {
        if (collapseZero)
        {
            const auto prev = summary_.value();
            auto reconcile = [](Nui::Observed<bool>& collapsed,
                                std::uint64_t prevCount,
                                std::uint64_t newCount) {
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

        // Clear per-section progress — rows are about to be re-fetched and any
        // in-flight row observers won't match the new generation anyway.
        uploadProgress_.clear();
        downloadProgress_.clear();
        deleteProgress_.clear();

        seedRootNodes();
    }

    /** @brief Fetches root-level children for every section and, once each
     *         reply lands, hands them to @ref Tree::setRoots and seeds that
     *         section's sparse selection set to the root relKeys (= "all
     *         selected" in sparsest form).
     */
    void seedRootNodes()
    {
        if (!provider_)
            return;
        auto seedOne = [this](DiffSection section) {
            provider_->loadChildren(
                section,
                std::string{},
                [this, section](std::vector<DiffTreeNode> nodes) {
                    std::vector<ScriptNuiComponents::Tree::Node> roots;
                    roots.reserve(nodes.size());
                    for (auto& node : nodes)
                        roots.push_back(toTreeNode(std::move(node)));
                    treeFor(section).setRoots(std::move(roots));
                    auto selectedPtr = selectionFor(section);
                    auto& set = selectedPtr->value();
                    set.clear();
                    for (auto const& rootId : treeFor(section).childrenOf(std::string{}))
                        set.insert(rootId);
                    selectedPtr->modify();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
                [](std::string const& msg) {
                    Log::error("Sync seedRootNodes failed: {}", msg);
                }
            );
        };
        seedOne(DiffSection::Upload);
        seedOne(DiffSection::Download);
        seedOne(DiffSection::Delete);
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

    /** @brief Triggered on any settings change — runs a fresh backend diff and
     *         reseeds the trees.  Collapses sections that zero out, expands ones
     *         that gain rows.
     */
    void recomputeDiff()
    {
        if (!provider_)
            return;
        provider_->recompute(
            buildDiffOptions(),
            [this](SharedData::Sync::DiffSummary summary) {
                applySummaryAndReseed(std::move(summary), /*collapseZero=*/true);
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        );
    }

    /** @brief Allocates a shared progress observer and propagates it to @p relKey
     *         plus every currently-loaded descendant row in the tree.  Used for
     *         bulk-directory enqueue where the whole subtree animates off a
     *         single backend progress stream.
     */
    ProgressObserver installProgressFor(DiffSection section, std::string const& relKey)
    {
        auto& progMap = progressMapFor(section);
        auto& entry = progMap[relKey];
        if (!entry)
            entry = std::make_shared<Nui::Observed<double>>(0.0);
        // Propagate to any already-loaded descendants so their rows share the
        // same gradient.  Descendants loaded AFTER enqueue inherit automatically
        // via the row renderer's prefix lookup below.
        const std::string prefix = relKey + "/";
        for (auto& [key, obs] : progMap)
        {
            if (key.size() > prefix.size() && key.starts_with(prefix))
                obs = entry;
        }
        return entry;
    }

    /** @brief Looks up the progress observer that applies to @p relKey — its own
     *         if present, otherwise the nearest ancestor's.  Returns nullptr when
     *         the row has no enqueued transfer affecting it.
     */
    ProgressObserver progressForRow(DiffSection section, std::string const& relKey) const
    {
        auto const& progMap =
            section == DiffSection::Upload ? uploadProgress_ :
            section == DiffSection::Download ? downloadProgress_ :
                                               deleteProgress_;
        if (auto iter = progMap.find(relKey); iter != progMap.end())
            return iter->second;
        // Walk ancestor prefixes.
        std::string candidate = relKey;
        while (!candidate.empty())
        {
            const auto slash = candidate.rfind('/');
            if (slash == std::string::npos)
                candidate.clear();
            else
                candidate.resize(slash);
            if (auto iter = progMap.find(candidate); iter != progMap.end())
                return iter->second;
        }
        return nullptr;
    }

    /**
     * @brief Enqueues a single row.  The row click path has the full DiffTreeNode
     *        in hand (via Tree row userData) so we don't need a provider round-trip.
     */
    void enqueueSingleNode(DiffSection section, DiffTreeNode const& node)
    {
        auto progress = installProgressFor(section, node.relKey);
        ++progressEpoch_.value();
        progressEpoch_.modify();
        Nui::globalEventContext.executeActiveEventsImmediately();

        const bool dirOnly = !recursive_.value() && node.isDirectory;
        auto onDirCreated = [progress](bool success, std::string const&) {
            *progress = success ? 1.1 : -1.0;
            Nui::globalEventContext.executeActiveEventsImmediately();
        };
        auto onComplete = [this, progress](std::optional<Ids::OperationId> const& opId, std::string const&) {
            if (!opId)
            {
                *progress = -1.0;
                Nui::globalEventContext.executeActiveEventsImmediately();
                return;
            }
            operationQueue_->addTransferProgressCallback(*opId, [progress](double fraction) {
                *progress = fraction;
                Nui::globalEventContext.executeActiveEventsImmediately();
            });
            operationQueue_->addCompletionCallback(*opId, [progress](bool) {
                *progress = 1.1;
                Nui::globalEventContext.executeActiveEventsImmediately();
            });
        };

        const auto localAbs = localPath_ / node.relKey;
        const auto remoteAbs = remotePath_ / node.relKey;

        switch (node.action)
        {
            case SharedData::Sync::Action::Upload:
            {
                if (dirOnly)
                {
                    operationQueue_->createRemoteDirectory(remoteAbs, onDirCreated);
                    return;
                }
                SharedData::DirectoryEntry localEntry{};
                localEntry.path = std::filesystem::path{node.relKey};
                localEntry.fullPath = localAbs;
                localEntry.size = node.localSize;
                localEntry.mtime = node.localMtime;
                localEntry.type = node.isDirectory ? SharedData::FileType::Directory : SharedData::FileType::Regular;
                SharedData::DirectoryEntry remoteEntry = localEntry;
                remoteEntry.fullPath = remoteAbs;
                operationQueue_->enqueueUpload(
                    NuiFileExplorer::Item{remoteEntry},
                    NuiFileExplorer::Item{localEntry},
                    onComplete,
                    /*allowOverwrite=*/true,
                    /*insertRefresh=*/false,
                    /*createMissingDirs=*/true,
                    SharedData::OperationMode::PriorityQueued
                );
                return;
            }
            case SharedData::Sync::Action::Download:
            {
                if (dirOnly)
                {
                    operationQueue_->createLocalDirectory(localAbs, onDirCreated);
                    return;
                }
                SharedData::DirectoryEntry remoteEntry{};
                remoteEntry.path = std::filesystem::path{node.relKey};
                remoteEntry.fullPath = remoteAbs;
                remoteEntry.size = node.remoteSize;
                remoteEntry.mtime = node.remoteMtime;
                remoteEntry.type = node.isDirectory ? SharedData::FileType::Directory : SharedData::FileType::Regular;
                SharedData::DirectoryEntry localEntry = remoteEntry;
                localEntry.fullPath = localAbs;
                operationQueue_->enqueueDownload(
                    NuiFileExplorer::Item{remoteEntry},
                    NuiFileExplorer::Item{localEntry},
                    onComplete,
                    /*allowOverwrite=*/true,
                    /*insertRefresh=*/false,
                    /*createMissingDirs=*/true,
                    SharedData::OperationMode::PriorityQueued
                );
                return;
            }
            case SharedData::Sync::Action::DeleteLocal:
            case SharedData::Sync::Action::DeleteRemote:
            {
                std::vector<std::filesystem::path> paths;
                paths.push_back(node.action == SharedData::Sync::Action::DeleteRemote ? remoteAbs : localAbs);
                operationQueue_->enqueueDelete(
                    paths,
                    recursive_.value(),
                    [progress](auto const& opIds, std::string const&) {
                        *progress = opIds ? 1.1 : -1.0;
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    },
                    SharedData::OperationMode::PriorityQueued
                );
                return;
            }
        }
    }

    void enqueueOperations()
    {
        if (!provider_)
            return;
        enqueueOneSection(DiffSection::Upload);
        enqueueOneSection(DiffSection::Download);
        enqueueOneSection(DiffSection::Delete);
    }

    /** @brief Asks the backend to collapse the section's selected relKeys into a
     *         minimal enqueue plan, then dispatches each plan entry.  Bulk files
     *         are batched into a BulkUpload/BulkDownload; directory-only entries
     *         (non-recursive mode) become single createRemote/LocalDirectory
     *         calls; deletes go to enqueueBulkDelete.
     */
    void enqueueOneSection(DiffSection section)
    {
        auto const& selectedSet = selectionFor(section)->value();
        std::vector<std::string> selected{selectedSet.begin(), selectedSet.end()};
        if (selected.empty())
            return;

        provider_->buildEnqueuePlan(
            section,
            std::move(selected),
            [this, section](std::vector<SharedData::Sync::EnqueuePlanEntry> plan) {
                dispatchPlan(section, std::move(plan));
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
            [](std::string const& msg) {
                Log::error("buildSyncEnqueuePlan failed: {}", msg);
            }
        );
    }

    void dispatchPlan(DiffSection section, std::vector<SharedData::Sync::EnqueuePlanEntry> plan)
    {
        if (plan.empty())
            return;

        using ProgressObserverVec = std::vector<ProgressObserver>;

        const bool nonRecursive = !recursive_.value();
        auto onDirCreatedFor = [](ProgressObserver prog) {
            return [prog](bool success, std::string const&) {
                *prog = success ? 1.1 : -1.0;
                Nui::globalEventContext.executeActiveEventsImmediately();
            };
        };

        std::vector<SharedData::BulkAddEntry> uploadEntries;
        ProgressObserverVec uploadObservers;
        std::vector<SharedData::BulkAddEntry> downloadEntries;
        ProgressObserverVec downloadObservers;
        std::vector<SharedData::BulkAddEntry> deleteEntries;
        ProgressObserverVec deleteObservers;

        for (auto& entry : plan)
        {
            auto progress = installProgressFor(section, entry.relKey);

            switch (entry.action)
            {
                case SharedData::Sync::Action::Upload:
                {
                    if (nonRecursive && entry.isDirectory)
                    {
                        operationQueue_->createRemoteDirectory(
                            std::filesystem::path{entry.remoteAbsPath}, onDirCreatedFor(progress)
                        );
                        continue;
                    }
                    uploadEntries.push_back(SharedData::BulkAddEntry{
                        .src = entry.localAbsPath,
                        .dst = entry.remoteAbsPath,
                        .sizeBytes = entry.sizeBytes,
                        .isDirectory = entry.isDirectory,
                        .mtime = entry.mtime,
                        .mtimeNsec = entry.mtimeNsec,
                    });
                    uploadObservers.push_back(progress);
                    break;
                }
                case SharedData::Sync::Action::Download:
                {
                    if (nonRecursive && entry.isDirectory)
                    {
                        operationQueue_->createLocalDirectory(
                            std::filesystem::path{entry.localAbsPath}, onDirCreatedFor(progress)
                        );
                        continue;
                    }
                    downloadEntries.push_back(SharedData::BulkAddEntry{
                        .src = entry.remoteAbsPath,
                        .dst = entry.localAbsPath,
                        .sizeBytes = entry.sizeBytes,
                        .isDirectory = entry.isDirectory,
                        .mtime = entry.mtime,
                        .mtimeNsec = entry.mtimeNsec,
                    });
                    downloadObservers.push_back(progress);
                    break;
                }
                case SharedData::Sync::Action::DeleteLocal:
                {
                    deleteEntries.push_back(SharedData::BulkAddEntry{
                        .src = entry.localAbsPath,
                        .dst = {},
                        .sizeBytes = 0,
                        .isDirectory = entry.isDirectory,
                    });
                    deleteObservers.push_back(progress);
                    break;
                }
                case SharedData::Sync::Action::DeleteRemote:
                {
                    deleteEntries.push_back(SharedData::BulkAddEntry{
                        .src = entry.remoteAbsPath,
                        .dst = {},
                        .sizeBytes = 0,
                        .isDirectory = entry.isDirectory,
                    });
                    deleteObservers.push_back(progress);
                    break;
                }
            }
        }

        ++progressEpoch_.value();
        progressEpoch_.modify();

        if (!uploadEntries.empty())
            hookBulkTransfer(std::move(uploadEntries), std::move(uploadObservers), /*isUpload=*/true, "upload");
        if (!downloadEntries.empty())
            hookBulkTransfer(std::move(downloadEntries), std::move(downloadObservers), /*isUpload=*/false, "download");

        if (!deleteEntries.empty())
        {
            auto observersShared = std::make_shared<ProgressObserverVec>(std::move(deleteObservers));
            operationQueue_->enqueueBulkDelete(
                std::move(deleteEntries),
                /*insertRefresh=*/false,
                SharedData::OperationMode::Queued,
                [observersShared](bool success) {
                    for (auto& obs : *observersShared)
                        if (obs) *obs = success ? 1.1 : -1.0;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
                [](bool success, std::string const& info) {
                    if (!success)
                        Log::error("Sync bulk delete failed: {}", info);
                }
            );
        }
    }

    /** @brief Shared wiring between BulkUpload and BulkDownload.  Tracks per-file
     *         progress via the backend's BulkProgress event stream and marks
     *         completed rows green.
     */
    void hookBulkTransfer(
        std::vector<SharedData::BulkAddEntry> entries,
        std::vector<ProgressObserver> observers,
        bool isUpload,
        std::string const& kind
    )
    {
        auto fileEntryIndices = std::make_shared<std::vector<std::size_t>>();
        fileEntryIndices->reserve(entries.size());
        for (std::size_t idx = 0; idx < entries.size(); ++idx)
            if (!entries[idx].isDirectory)
                fileEntryIndices->push_back(idx);

        auto entryIsDir = std::make_shared<std::vector<bool>>();
        entryIsDir->reserve(entries.size());
        for (auto const& entry : entries)
            entryIsDir->push_back(entry.isDirectory);
        auto observersShared = std::make_shared<std::vector<ProgressObserver>>(std::move(observers));

        auto markFilesCompletedBefore =
            [observersShared, fileEntryIndices](std::uint64_t upTo) {
                const auto limit = std::min<std::uint64_t>(upTo, fileEntryIndices->size());
                for (std::uint64_t pos = 0; pos < limit; ++pos)
                {
                    auto& obs = (*observersShared)[(*fileEntryIndices)[pos]];
                    if (!obs)
                        continue;
                    const double value = obs->value();
                    if (value >= 1.0 || value < 0.0)
                        continue;
                    *obs = 1.1;
                }
            };

        auto onBulkProgress =
            [observersShared, fileEntryIndices, markFilesCompletedBefore](SharedData::BulkProgress const& prog) {
                markFilesCompletedBefore(prog.fileCurrentIndex);
                if (prog.fileCurrentIndex >= fileEntryIndices->size())
                {
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    return;
                }
                auto& obs = (*observersShared)[(*fileEntryIndices)[prog.fileCurrentIndex]];
                if (obs && prog.currentFileTotalBytes > 0)
                {
                    *obs = static_cast<double>(prog.currentFileBytes)
                        / static_cast<double>(prog.currentFileTotalBytes);
                }
                Nui::globalEventContext.executeActiveEventsImmediately();
            };

        auto onEnqueued = [this,
                           onBulkProgress = std::move(onBulkProgress),
                           observersShared,
                           entryIsDir](std::vector<Ids::OperationId> const& opIds) {
            if (opIds.empty())
                return;
            operationQueue_->addBulkProgressCallback(opIds.back(), onBulkProgress);
            operationQueue_->addCompletionCallback(
                opIds.back(),
                [observersShared, entryIsDir](bool success) {
                    for (std::size_t idx = 0; idx < observersShared->size(); ++idx)
                    {
                        if (idx < entryIsDir->size() && (*entryIsDir)[idx])
                            continue;
                        auto& obs = (*observersShared)[idx];
                        if (obs)
                            *obs = success ? 1.1 : -1.0;
                    }
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
            for (std::size_t idx = 0; idx < opIds.size() && idx < entryIsDir->size(); ++idx)
            {
                if (!(*entryIsDir)[idx])
                    continue;
                auto observer = (idx < observersShared->size()) ? (*observersShared)[idx] : nullptr;
                if (!observer)
                    continue;
                operationQueue_->addCompletionCallback(
                    opIds[idx],
                    [observer](bool success) {
                        *observer = success ? 1.1 : -1.0;
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                );
            }
        };

        auto onBulkAck = [kind](bool success, std::string const& info) {
            if (!success)
                Log::error("Sync bulk {} failed: {}", kind, info);
        };

        if (isUpload)
        {
            operationQueue_->enqueueBulkUpload(
                std::move(entries), /*allowOverwrite*/ true, /*insertRefresh*/ false,
                SharedData::OperationMode::Queued,
                /*onEachComplete*/ {}, std::move(onBulkAck), std::move(onEnqueued)
            );
        }
        else
        {
            operationQueue_->enqueueBulkDownload(
                std::move(entries), /*allowOverwrite*/ true, /*insertRefresh*/ false,
                SharedData::OperationMode::Queued,
                /*onEachComplete*/ {}, std::move(onBulkAck), std::move(onEnqueued)
            );
        }
    }
};

// ---- Tree integration ------------------------------------------------------

ScriptNuiComponents::Tree::RowContentRenderer SyncDialog::Implementation::makeTreeRowRenderer(DiffSection section)
{
    return [this, section](ScriptNuiComponents::Tree::RowContext const& ctx) -> Nui::ElementRenderer {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        DiffTreeNode const* nodePtr = userDataAsDiffTreeNode(ctx.userData);
        if (!nodePtr)
        {
            // Fallback for any directory row the Tree synthesizes without userData
            // (shouldn't happen now that the backend emits directory DiffTreeNodes,
            // but the guard keeps rendering robust).
            std::string_view view{ctx.id};
            if (!view.empty() && view.back() == '/')
                view.remove_suffix(1);
            const auto slash = view.rfind('/');
            const auto basename = (slash == std::string_view::npos) ? view : view.substr(slash + 1);
            return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}(
                    span{class_ = "name"}(std::string{basename})
                ),
                div{class_ = "sync-diff-arrow"}(),
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}()
            );
        }

        DiffTreeNode const& node = *nodePtr;

        // Structural rows (synthesized ancestors of deep diffs) have no action
        // and no transfer of their own — render a pure folder label with no arrow.
        if (node.isStructural)
        {
            return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}(
                    span{class_ = "name"}(node.name)
                ),
                div{class_ = "sync-diff-arrow"}(),
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}()
            );
        }

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

        const DiffTreeNode nodeCopy = node; // captured into arrow click handler
        auto makeArrow = [&, nodeCopy]() {
            return div{
                class_ = fmt::format("sync-diff-arrow sync-diff-arrow--clickable {}", arrowClass),
                "title"_attr = language->get("syncDialog", "syncItemNowTitle"),
                onClick = [this, section, nodeCopy](Nui::val event) {
                    event.call<void>("stopPropagation");
                    enqueueSingleNode(section, nodeCopy);
                },
            }(std::move(arrowIcon));
        };

        // Directory rows (one-sided subtree emits): render a folder-style cell on
        // whichever side holds the data.
        if (node.isDirectory)
        {
            const bool labelRight = actionContentOnRight(node.action);
            auto labelCell = span{
                class_ = labelRight
                    ? "sync-diff-cell sync-diff-cell-right sync-diff-cell--directory"
                    : "sync-diff-cell sync-diff-cell--directory"
            }(span{class_ = "name"}(node.name));
            auto emptyCell = span{class_ = "sync-diff-cell sync-diff-cell--directory"}();
            if (labelRight)
            {
                return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                    std::move(emptyCell),
                    makeArrow(),
                    std::move(labelCell)
                );
            }
            return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                std::move(labelCell),
                makeArrow(),
                std::move(emptyCell)
            );
        }

        // Leaf row — progress gradient driven by whichever observer matches this
        // relKey (own or nearest ancestor's).  Repaint when progressEpoch_ moves.
        auto progress = progressForRow(section, node.relKey);
        if (progress)
        {
            auto prog = progress;
            return div{
                class_ = "sync-diff-row",
                style = Nui::observe(*prog).generate([prog]() -> std::string {
                    const double val = prog->value();
                    if (val > 1.0)
                        return "--sync-row-bg: var(--sync-done-color, rgba(76,175,80,0.18));";
                    if (val < 0.0)
                        return "--sync-row-bg: var(--sync-error-color, rgba(231,76,60,0.18));";
                    const int pct = static_cast<int>(val * 100.0);
                    return fmt::format(
                        "--sync-row-bg: linear-gradient(to right,"
                        " var(--sync-progress-color, rgba(76,175,80,0.25)) {}%,"
                        " transparent {}%);",
                        pct, pct);
                })
            }(
                renderCellFromNode(node, /*isRemote=*/false, /*alignRight=*/false),
                makeArrow(),
                renderCellFromNode(node, /*isRemote=*/true, /*alignRight=*/true)
            );
        }

        return div{class_ = "sync-diff-row"}(
            renderCellFromNode(node, /*isRemote=*/false, /*alignRight=*/false),
            makeArrow(),
            renderCellFromNode(node, /*isRemote=*/true, /*alignRight=*/true)
        );
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

void SyncDialog::open(
    BackendSyncProvider* provider,
    SharedData::Sync::DiffSummary summary,
    std::filesystem::path localPath,
    std::filesystem::path remotePath
)
{
    impl_->provider_ = provider;
    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);

    impl_->applySummaryAndReseed(std::move(summary), /*collapseZero=*/false);

    impl_->uploadCollapsed_ = impl_->summary_.value().uploads.itemCount == 0;
    impl_->downloadCollapsed_ = impl_->summary_.value().downloads.itemCount == 0;
    impl_->deleteCollapsed_ = impl_->summary_.value().deletes.itemCount == 0;

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

    auto onSettingChange = [this]() {
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
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->respectIgnore_ = val; onSettingChange();
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

            // Footer
            div{class_ = "sync-dialog-footer"}(
                div{class_ = "sync-footer-actions"}(
                    Snc::button({
                        .text = language->getObserved("syncDialog", "recompare"),
                        .icon = Ui5Icons::refresh(),
                        .attributes = {
                            onClick = [this](Nui::val) {
                                if (impl_->onRecompareRequested_)
                                    impl_->onRecompareRequested_(RecompareRequest{
                                        .respectIgnoreFiles = impl_->respectIgnore_.value(),
                                        .recursive = impl_->recursive_.value(),
                                        .ignoreHidden = impl_->ignoreHidden_.value(),
                                        .diffOptions = impl_->buildDiffOptions(),
                                    });
                            }
                        }
                    }),
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
                            // We don't know per-leaf sizes without walking the loaded rows, so
                            // the footer shows the selected-count across sections and the
                            // summed section transferBytes as an approximation when the
                            // user selects all.  For partial selections this undercounts
                            // only when a one-sided bulk-directory is partially deselected
                            // — an edge case the backend planner handles correctly.
                            auto const& s = impl_->summary_.value();
                            const auto selectedCount =
                                impl_->uploadSelected_->value().size()
                                + impl_->downloadSelected_->value().size()
                                + impl_->deleteSelected_->value().size();
                            const auto totalBytes =
                                s.uploads.transferBytes + s.downloads.transferBytes + s.deletes.transferBytes;
                            return span{}(fmt::format(
                                fmt::runtime(language->get("syncDialog", "footerSummary")),
                                selectedCount,
                                Utility::formatBytes(static_cast<long long>(totalBytes))
                            ));
                        }
                    ),
                    Snc::button({
                        .text = language->getObserved("syncDialog", "synchronize"),
                        .icon = Ui5Icons::synchronize(),
                        .attributes = {
                            onClick = [this](Nui::val) {
                                impl_->confirmDialog_->open({
                                    .styleVariant = Snc::StyleVariant::Warning,
                                    .headerText = language->get("syncDialog", "confirmHeader"),
                                    .text = fmt::format(
                                        fmt::runtime(language->get("syncDialog", "confirmText")),
                                        impl_->localPath_.filename().string(),
                                        impl_->remotePath_.filename().string()
                                    ),
                                    .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
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
