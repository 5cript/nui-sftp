#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/sync_item.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>

#include <utility/language.hpp>

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
#include <script-nui-components/tree_fold.hpp>

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

#include <shared_data/directory_entry.hpp>
#include <shared_data/sync_phase.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

using namespace std::string_literals;

namespace
{
    enum class SyncDirection
    {
        Both,
        Upload,
        Download
    };

    std::string formatMtime(std::uint64_t mtime)
    {
        using namespace std::chrono;
        const auto tp = system_clock::time_point{seconds{static_cast<long long>(mtime)}};
        return fmt::format("{:%Y-%m-%d}", floor<days>(tp));
    }

    struct SyncTotals
    {
        std::size_t count{0};
        std::uint64_t bytes{0};
    };

    /** @brief Returns the byte count of the side that's actually being transferred
     *         for @p itm.  Directories (size==0) and missing sides contribute 0.
     */
    std::uint64_t transferBytes(SyncItem const& itm)
    {
        switch (itm.action)
        {
            case SyncItemAction::Upload:
                return itm.localItem ? itm.localItem->size : 0ull;
            case SyncItemAction::Download:
                return itm.remoteItem ? itm.remoteItem->size : 0ull;
            case SyncItemAction::DeleteLocal:
                return itm.localItem ? itm.localItem->size : 0ull;
            case SyncItemAction::DeleteRemote:
                return itm.remoteItem ? itm.remoteItem->size : 0ull;
        }
        return 0ull;
    }

    /** @brief Sums item count + transfer bytes for @p items.  When @p selected
     *         is non-null, only items whose relKey is in the set contribute (the
     *         set holds leaf relKeys only — directory rows naturally get skipped).
     */
    SyncTotals computeTotals(
        std::vector<SyncItem> const& items,
        std::unordered_set<std::string> const* selected = nullptr
    )
    {
        SyncTotals total{};
        for (auto const& itm : items)
        {
            if (selected && !selected->contains(itm.relKey))
                continue;
            ++total.count;
            total.bytes += transferBytes(itm);
        }
        return total;
    }

    Nui::ElementRenderer renderItemCell(std::optional<NuiFileExplorer::Item> const& item, bool alignRight)
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using namespace Nui::Attributes::Literals;
        using Nui::Elements::div;
        using Nui::Elements::span;

        if (!item)
        {
            return div{class_ = fmt::format("sync-diff-cell empty {}", alignRight ? "sync-diff-cell-right" : "")}();
        }

        const auto& entry = *item;
        const auto sizeStr = entry.type == SharedData::FileType::Directory
            ? std::string{}
            : Utility::formatBytes(static_cast<long long>(entry.size));
        const auto mtimeStr = entry.mtime > 0 ? formatMtime(entry.mtime) : std::string{};
        // The tree already communicates hierarchy via indent + chevron; showing
        // the full relative path here just duplicates that and wastes row space.
        const auto name = entry.path.filename().generic_string();

        return div{
            class_ = fmt::format("sync-diff-cell {}", alignRight ? "sync-diff-cell-right" : ""),
            "title"_attr = entry.fullPath.string()
        }(
            span{class_ = "name"}(name),
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

    /** @brief Build a flat map of relative-path string → entry for a scan result.
     *         The root entry (index 0) is excluded from the map.
     *
     * @param root    The absolute root path that was scanned.
     * @param entries Flat entry list with pre-computed fullPath fields.
     */
    std::map<std::string, SharedData::DirectoryEntry>
    buildEntryMap(std::filesystem::path const& root, std::vector<SharedData::DirectoryEntry> const& entries)
    {
        std::map<std::string, SharedData::DirectoryEntry> result;
        for (std::size_t idx = 1; idx < entries.size(); ++idx)
        {
            const auto& entry = entries[idx];
            std::filesystem::path relPath;
            if (entry.fullPath.has_relative_path())
            {
                relPath = std::filesystem::path{entry.fullPath.generic_string()}.lexically_relative(
                    std::filesystem::path{root.generic_string()}
                );
            }
            else
            {
                relPath = entry.path;
            }
            if (!relPath.empty())
                result.emplace(relPath.generic_string(), entry);
        }
        return result;
    }

    /** @brief Returns true when the two entries are considered different (by size and mtime).
     *
     * Symlinks are compared by raw link target (readlink value): two links with the same target
     * are in sync, two with different targets are not — size and mtime of the link itself are
     * meaningless for comparison. A type mismatch (symlink vs regular file) always counts as a diff.
     */
    bool entriesDiffer(SharedData::DirectoryEntry const& loc, SharedData::DirectoryEntry const& rem)
    {
        if (loc.type != rem.type)
            return true;
        if (loc.type == SharedData::FileType::Symlink)
        {
            if (loc.linkTarget && rem.linkTarget)
                return *loc.linkTarget != *rem.linkTarget;
            return false;
        }
        if (loc.size != rem.size)
            return true;
        if (loc.mtime != rem.mtime)
            return true;
        return false;
    }

}

// ---- Implementation ---------------------------------------------------------

struct SyncDialog::Implementation
{
    Nui::Observed<bool> open_{false};
    // When true, the dialog DOM stays mounted with full state intact but is
    // hidden; a restore button in the OperationQueue header brings it back.
    Nui::Observed<bool> minimized_{false};
    std::filesystem::path localPath_{};
    std::filesystem::path remotePath_{};

    // Cached scan results (set by open())
    std::vector<SharedData::DirectoryEntry> localEntries_{};
    std::vector<SharedData::DirectoryEntry> remoteEntries_{};

    // Settings
    Nui::Observed<std::string> directionStr_{"Both"s};
    SyncDirection direction_{SyncDirection::Both};
    Nui::Observed<bool> respectIgnore_{true};
    Nui::Observed<bool> recursive_{true};
    Nui::Observed<bool> ignoreHidden_{false};
    Nui::Observed<bool> actionUpload_{true};
    Nui::Observed<bool> actionDownload_{true};
    Nui::Observed<bool> actionDelete_{false};

    // Diff item lists
    Nui::Observed<std::vector<SyncItem>> uploadItems_{};
    Nui::Observed<std::vector<SyncItem>> downloadItems_{};
    Nui::Observed<std::vector<SyncItem>> deleteItems_{};

    // Per-tree selection sets (leaf NodeIds only; directory tristate is
    // computed by the tree from these).  Shared with the tree Options.
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> uploadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> downloadSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};
    std::shared_ptr<Nui::Observed<std::unordered_set<std::string>>> deleteSelected_{
        std::make_shared<Nui::Observed<std::unordered_set<std::string>>>()};

    // One tree per diff list; holds per-node expansion state across recompares.
    // Options (row renderer etc.) are built in the ctor and never reassigned.
    ScriptNuiComponents::Tree uploadTree_{};
    ScriptNuiComponents::Tree downloadTree_{};
    ScriptNuiComponents::Tree deleteTree_{};

    Nui::Observed<bool> uploadCollapsed_{false};
    Nui::Observed<bool> downloadCollapsed_{false};
    Nui::Observed<bool> deleteCollapsed_{false};

    ConfirmDialog* confirmDialog_;
    OperationQueue* operationQueue_;
    std::function<void(
        std::filesystem::path,
        std::filesystem::path,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(
            std::vector<SharedData::DirectoryEntry>,
            std::vector<SharedData::DirectoryEntry>
        )>
    )>
        onRecompare_{};

    explicit Implementation(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
        : confirmDialog_{confirmDialog}
        , operationQueue_{operationQueue}
    {
        directionStr_ = language->get("syncDialog", "directionBoth");
        initTrees();
    }

    void initTrees()
    {
        namespace Snc = ScriptNuiComponents;
        uploadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(uploadItems_, /*mirrored=*/false),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = true,
            .showIcons = false,
            .selected = uploadSelected_,
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
        downloadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(downloadItems_, /*mirrored=*/true),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = true,
            .showIcons = false,
            .mirror = true,
            .selected = downloadSelected_,
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
        deleteTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(deleteItems_, /*mirrored=*/false),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = true,
            .showIcons = false,
            .selected = deleteSelected_,
            .showCollapseAllButton = true,
            .showSelectAllButton = true,
            .showDeselectAllButton = true,
        }};
    }

    ScriptNuiComponents::Tree::RowContentRenderer
    makeTreeRowRenderer(Nui::Observed<std::vector<SyncItem>>& listObs, bool mirrored);

    ScriptNuiComponents::Tree::RowAttributeProvider makeTreeRowAttributes();

    /** @brief Feeds the current item vectors into the trees.  Called after any
     *         modification of uploadItems_/downloadItems_/deleteItems_.  The
     *         trees' keyed merge preserves per-node expansion state.
     */
    void refreshTrees();

    void enqueueSingleByRelKey(Nui::Observed<std::vector<SyncItem>>& list, std::string const& relKey);

    void recomputeDiff()
    {
        std::vector<SyncItem> uploads;
        std::vector<SyncItem> downloads;
        std::vector<SyncItem> deletes;

        if (localEntries_.empty() && remoteEntries_.empty())
        {
            uploadItems_ = std::move(uploads);
            downloadItems_ = std::move(downloads);
            deleteItems_ = std::move(deletes);
            refreshTrees();
            return;
        }

        auto localMap = buildEntryMap(localPath_, localEntries_);
        auto remoteMap = buildEntryMap(remotePath_, remoteEntries_);

        if (ignoreHidden_.value())
        {
            // Backend already filters when this toggle is on at scan-time; this
            // post-scan pass exists so toggling the switch ON without a
            // recompare also hides any hidden entries from a prior scan.  A
            // hidden segment anywhere in the relKey hides the entry (".git/"
            // hides the whole tree, not just the top-level dotfile).
            const auto hasHiddenSegment = [](std::string const& relKey) {
                std::size_t pos = 0;
                while (pos < relKey.size())
                {
                    if (relKey[pos] == '.')
                        return true;
                    pos = relKey.find('/', pos);
                    if (pos == std::string::npos)
                        break;
                    ++pos;
                }
                return false;
            };
            for (auto mapIt = localMap.begin(); mapIt != localMap.end();)
                mapIt = hasHiddenSegment(mapIt->first) ? localMap.erase(mapIt) : std::next(mapIt);
            for (auto mapIt = remoteMap.begin(); mapIt != remoteMap.end();)
                mapIt = hasHiddenSegment(mapIt->first) ? remoteMap.erase(mapIt) : std::next(mapIt);
        }

        if (!recursive_.value())
        {
            // Non-recursive mode: drop everything below the root directory.  The scans
            // themselves always recurse (cheaper than a separate top-level-only scan path);
            // we filter here so toggling the switch is immediate and does not require a
            // new recompare.
            const auto isNested = [](std::string const& relKey) {
                return relKey.find('/') != std::string::npos;
            };
            for (auto mapIt = localMap.begin(); mapIt != localMap.end();)
                mapIt = isNested(mapIt->first) ? localMap.erase(mapIt) : std::next(mapIt);
            for (auto mapIt = remoteMap.begin(); mapIt != remoteMap.end();)
                mapIt = isNested(mapIt->first) ? remoteMap.erase(mapIt) : std::next(mapIt);
        }

        auto makeLocalItem = [](SharedData::DirectoryEntry const& entry, std::string const& relKey) {
            SharedData::DirectoryEntry item = entry;
            item.path = std::filesystem::path{relKey};
            return NuiFileExplorer::Item{item};
        };
        auto makeRemoteItem = [](SharedData::DirectoryEntry const& entry, std::string const& relKey) {
            SharedData::DirectoryEntry item = entry;
            item.path = std::filesystem::path{relKey};
            return NuiFileExplorer::Item{item};
        };

        auto makeItem = [](SyncItemAction action, std::optional<NuiFileExplorer::Item> localI,
                           std::optional<NuiFileExplorer::Item> remoteI, std::string relKey) {
            SyncItem item{};
            item.action = action;
            item.localItem = std::move(localI);
            item.remoteItem = std::move(remoteI);
            item.relKey = std::move(relKey);
            return item;
        };

        // --- Entries that exist locally ---
        for (auto const& [relKey, localEntry] : localMap)
        {
            auto remIt = remoteMap.find(relKey);
            if (remIt == remoteMap.end())
            {
                if (actionUpload_.value() && direction_ != SyncDirection::Download)
                    uploads.push_back(
                        makeItem(SyncItemAction::Upload, makeLocalItem(localEntry, relKey), std::nullopt, relKey));
                else if (actionDelete_.value() && direction_ == SyncDirection::Download)
                {
                    // In non-recursive mode we haven't scanned the directory's
                    // children, so we cannot know whether deleting it would
                    // remove files the user never saw.  Hide the directory
                    // instead of risking a recursive delete.
                    const bool skipDir = !recursive_.value()
                        && localEntry.type == SharedData::FileType::Directory;
                    if (!skipDir)
                        deletes.push_back(makeItem(
                            SyncItemAction::DeleteLocal, makeLocalItem(localEntry, relKey), std::nullopt, relKey));
                }
            }
            else
            {
                const auto& remoteEntry = remIt->second;
                if (localEntry.type == SharedData::FileType::Directory)
                    continue;

                if (!entriesDiffer(localEntry, remoteEntry))
                    continue;

                const bool localNewer = localEntry.mtime >= remoteEntry.mtime;

                if (direction_ == SyncDirection::Upload ||
                    (direction_ == SyncDirection::Both && localNewer))
                {
                    if (actionUpload_.value())
                        uploads.push_back(makeItem(
                            SyncItemAction::Upload,
                            makeLocalItem(localEntry, relKey),
                            makeRemoteItem(remoteEntry, relKey),
                            relKey));
                }
                else
                {
                    if (actionDownload_.value())
                        downloads.push_back(makeItem(
                            SyncItemAction::Download,
                            makeLocalItem(localEntry, relKey),
                            makeRemoteItem(remoteEntry, relKey),
                            relKey));
                }
            }
        }

        // --- Entries that exist only remotely ---
        for (auto const& [relKey, remoteEntry] : remoteMap)
        {
            if (localMap.count(relKey))
                continue;

            if (actionDownload_.value() && direction_ != SyncDirection::Upload)
                downloads.push_back(
                    makeItem(SyncItemAction::Download, std::nullopt, makeRemoteItem(remoteEntry, relKey), relKey));
            else if (actionDelete_.value() && direction_ == SyncDirection::Upload)
            {
                // See matching comment on DeleteLocal: hide directories in
                // non-recursive mode instead of offering a potentially
                // destructive recursive delete.
                const bool skipDir = !recursive_.value()
                    && remoteEntry.type == SharedData::FileType::Directory;
                if (!skipDir)
                    deletes.push_back(makeItem(
                        SyncItemAction::DeleteRemote, std::nullopt, makeRemoteItem(remoteEntry, relKey), relKey));
            }
        }

        // Auto-toggle a section's collapsed state on N↔0 transitions so the
        // user isn't left looking at an empty open section after changing
        // direction/actions, and isn't surprised by a hidden newly-populated
        // section either.  Only zero-crossings flip — manual collapses while
        // a section stays populated are preserved.
        auto reconcileCollapse = [](Nui::Observed<bool>& collapsed,
                                    std::size_t prevCount,
                                    std::size_t newCount) {
            if (prevCount > 0 && newCount == 0)
                collapsed = true;
            else if (prevCount == 0 && newCount > 0)
                collapsed = false;
        };
        reconcileCollapse(uploadCollapsed_, uploadItems_.value().size(), uploads.size());
        reconcileCollapse(downloadCollapsed_, downloadItems_.value().size(), downloads.size());
        reconcileCollapse(deleteCollapsed_, deleteItems_.value().size(), deletes.size());

        uploadItems_ = std::move(uploads);
        downloadItems_ = std::move(downloads);
        deleteItems_ = std::move(deletes);
        resetSelectionAllChecked();
        refreshTrees();
    }

    /** @brief Resets each tree's selection set to contain every current tree-leaf
     *         relKey — the "all checked" default users expect after a recompare.
     */
    void resetSelectionAllChecked();

    /** @brief Enqueues a single item from one of the three diff lists at priority.
     *
     * @param list  The observed list the item belongs to (upload, download or delete).
     * @param index Index of the item inside @p list.
     */
    /** @brief True iff the SyncItem refers to a directory entry on either side.
     *         Directories only appear in the diff lists when missing on the
     *         opposite side; when the user has disabled recursion we must NOT
     *         dispatch them as bulk transfers (which would walk the local tree
     *         and could overwrite remote files the user never saw in the diff).
     */
    static bool isDirectoryItem(SyncItem const& itm)
    {
        if (itm.localItem && itm.localItem->type == SharedData::FileType::Directory)
            return true;
        if (itm.remoteItem && itm.remoteItem->type == SharedData::FileType::Directory)
            return true;
        return false;
    }

    void enqueueSingle(Nui::Observed<std::vector<SyncItem>>& list, std::size_t index)
    {
        auto items = list.value();
        if (index >= items.size())
            return;

        auto progress = std::make_shared<Nui::Observed<double>>(0.0);
        items[index].progress = progress;
        const auto itemCopy = items[index];
        list = std::move(items);
        refreshTrees();
        Nui::globalEventContext.executeActiveEventsImmediately();

        auto onComplete = [this, progress](std::optional<Ids::OperationId> const& opId, std::string const&)
        {
            if (!opId)
            {
                *progress = -1.0;
                Nui::globalEventContext.executeActiveEventsImmediately();
                return;
            }
            operationQueue_->addTransferProgressCallback(
                *opId,
                [progress](double fraction) {
                    *progress = fraction;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
            operationQueue_->addCompletionCallback(
                *opId,
                [progress](bool) {
                    *progress = 1.1;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
        };

        // In non-recursive mode a directory entry only ever means "create the
        // empty dir on the other side" — we have not scanned its children, so
        // a bulk transfer here would walk the local tree and could clobber
        // remote files the user never saw in the diff.
        const bool dirOnly = !recursive_.value() && isDirectoryItem(itemCopy);
        auto onDirCreated = [progress](bool success, std::string const&) {
            *progress = success ? 1.1 : -1.0;
            Nui::globalEventContext.executeActiveEventsImmediately();
        };

        switch (itemCopy.action)
        {
            case SyncItemAction::Upload:
            {
                if (dirOnly)
                {
                    operationQueue_->createRemoteDirectory(
                        remotePath_ / itemCopy.relKey, onDirCreated
                    );
                }
                else if (itemCopy.localItem && itemCopy.remoteItem)
                {
                    operationQueue_->enqueueUpload(
                        *itemCopy.remoteItem, *itemCopy.localItem, onComplete, true, true, /*createMissingDirs=*/true,
                        SharedData::OperationMode::PriorityQueued
                    );
                }
                else if (itemCopy.localItem)
                {
                    SharedData::DirectoryEntry remoteStub = *itemCopy.localItem;
                    remoteStub.path = itemCopy.localItem->path;
                    remoteStub.fullPath = remotePath_ / itemCopy.localItem->path;
                    operationQueue_->enqueueUpload(
                        NuiFileExplorer::Item{remoteStub}, *itemCopy.localItem, onComplete, true, true,
                        /*createMissingDirs=*/true, SharedData::OperationMode::PriorityQueued
                    );
                }
                break;
            }
            case SyncItemAction::Download:
            {
                if (dirOnly)
                {
                    operationQueue_->createLocalDirectory(
                        localPath_ / itemCopy.relKey, onDirCreated
                    );
                }
                else if (itemCopy.localItem && itemCopy.remoteItem)
                {
                    operationQueue_->enqueueDownload(
                        *itemCopy.remoteItem, *itemCopy.localItem, onComplete, true, true, /*createMissingDirs=*/true,
                        SharedData::OperationMode::PriorityQueued
                    );
                }
                else if (itemCopy.remoteItem)
                {
                    SharedData::DirectoryEntry localStub = *itemCopy.remoteItem;
                    localStub.path = itemCopy.remoteItem->path;
                    localStub.fullPath = localPath_ / itemCopy.remoteItem->path;
                    operationQueue_->enqueueDownload(
                        *itemCopy.remoteItem, NuiFileExplorer::Item{localStub}, onComplete, true, true,
                        /*createMissingDirs=*/true, SharedData::OperationMode::PriorityQueued
                    );
                }
                break;
            }
            case SyncItemAction::DeleteLocal:
            case SyncItemAction::DeleteRemote:
            {
                std::vector<std::filesystem::path> paths;
                if (itemCopy.action == SyncItemAction::DeleteRemote && itemCopy.remoteItem)
                    paths.push_back(itemCopy.remoteItem->fullPath);
                else if (itemCopy.action == SyncItemAction::DeleteLocal && itemCopy.localItem)
                    paths.push_back(itemCopy.localItem->fullPath);
                if (!paths.empty())
                {
                    operationQueue_->enqueueDelete(
                        paths, recursive_.value(),
                        [progress](auto const& opIds, std::string const&) {
                            *progress = opIds ? 1.1 : -1.0;
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        },
                        SharedData::OperationMode::PriorityQueued
                    );
                }
                break;
            }
        }
    }

    /** @brief Computes the minimal set of item indices to enqueue from @p items
     *         given the user's @p selected leaf set.
     *
     *  Directory-role items (one side is absent, the present side is a Directory)
     *  cause the backend to recursively transfer/delete the whole subtree.  When
     *  every descendant item is also selected, we emit just the directory and
     *  skip its descendants — the former behaviour of emitting the directory
     *  AND every descendant caused each leaf to be transferred N times, where N
     *  is its depth below the top-most directory-role ancestor.
     *
     *  When a directory has any deselected descendant, we cannot use its bulk
     *  operation (it would act on items the user excluded), so we recurse and
     *  emit only selected tree-leaves.  Intermediate directory-role items with
     *  partial selection are not emitted themselves: for Upload/Download the
     *  leaf enqueues use createMissingDirectories; for Delete the remaining
     *  non-empty dir is an acceptable no-op.
     */
    static std::vector<std::size_t> minimizeEnqueueIndices(
        std::vector<SyncItem> const& items,
        std::unordered_set<std::string> const& selected)
    {
        const std::size_t count = items.size();
        if (count == 0)
            return {};

        std::vector<bool> isTreeLeaf(count, true);
        for (std::size_t idx = 0; idx < count; ++idx)
        {
            const std::string prefix = items[idx].relKey + "/";
            for (std::size_t other = 0; other < count; ++other)
            {
                if (other == idx)
                    continue;
                if (items[other].relKey.size() > prefix.size() &&
                    items[other].relKey.starts_with(prefix))
                {
                    isTreeLeaf[idx] = false;
                    break;
                }
            }
        }

        const auto isBulkDir = [](SyncItem const& item) {
            switch (item.action)
            {
                case SyncItemAction::Download:
                    return !item.localItem && item.remoteItem &&
                        item.remoteItem->type == SharedData::FileType::Directory;
                case SyncItemAction::Upload:
                    return !item.remoteItem && item.localItem &&
                        item.localItem->type == SharedData::FileType::Directory;
                case SyncItemAction::DeleteLocal:
                    return item.localItem &&
                        item.localItem->type == SharedData::FileType::Directory;
                case SyncItemAction::DeleteRemote:
                    return item.remoteItem &&
                        item.remoteItem->type == SharedData::FileType::Directory;
            }
            return false;
        };

        // Leaf-keyed selection; for a dir-role item the "anySelected" /
        // "allSelected" questions reduce to scanning its descendant tree-leaves.
        const auto subtreeSelectionState = [&](std::size_t idx) {
            struct State { bool any{false}; bool all{true}; } state;
            if (isTreeLeaf[idx])
            {
                const bool checked = selected.contains(items[idx].relKey);
                state.any = checked;
                state.all = checked;
                return state;
            }
            const std::string prefix = items[idx].relKey + "/";
            bool sawAnyLeaf = false;
            for (std::size_t other = 0; other < count; ++other)
            {
                if (!isTreeLeaf[other])
                    continue;
                if (items[other].relKey.size() <= prefix.size() ||
                    !items[other].relKey.starts_with(prefix))
                    continue;
                sawAnyLeaf = true;
                const bool checked = selected.contains(items[other].relKey);
                state.any = state.any || checked;
                if (!checked)
                    state.all = false;
            }
            if (!sawAnyLeaf)
                state.all = false; // no descendants at all → don't treat as "all selected"
            return state;
        };

        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
            return items[lhs].relKey.size() < items[rhs].relKey.size();
        });

        std::vector<std::string> coveredPrefixes;
        std::vector<std::size_t> result;
        result.reserve(count);

        const auto isCovered = [&](std::string const& relKey) {
            for (auto const& ancestor : coveredPrefixes)
            {
                const std::string prefix = ancestor + "/";
                if (relKey.size() > prefix.size() && relKey.starts_with(prefix))
                    return true;
            }
            return false;
        };

        for (std::size_t idx : order)
        {
            auto const& item = items[idx];
            if (isCovered(item.relKey))
                continue;

            if (isBulkDir(item))
            {
                const auto state = subtreeSelectionState(idx);
                if (!state.any)
                    continue; // no selected work under this directory
                if (state.all)
                {
                    // Bulk-transfer the whole subtree in one operation.
                    result.push_back(idx);
                    coveredPrefixes.push_back(item.relKey);
                    continue;
                }
                // Partial selection: fall through; the dir itself is not
                // emitted, descendants will be considered individually.
                continue;
            }

            if (!isTreeLeaf[idx])
                continue; // non-directory intermediate node: defer to descendants
            if (!selected.contains(item.relKey))
                continue;

            result.push_back(idx);
        }

        return result;
    }

    void enqueueOperations()
    {
        const auto uploadIndices = minimizeEnqueueIndices(uploadItems_.value(), uploadSelected_->value());
        const auto downloadIndices = minimizeEnqueueIndices(downloadItems_.value(), downloadSelected_->value());
        const auto deleteIndices = minimizeEnqueueIndices(deleteItems_.value(), deleteSelected_->value());

        // Assign progress observers to selected items (and propagate to every
        // descendant SyncItem so tree rows reflect the bulk op's progress).
        // Progress observers must be in place before any RPC callback fires, so
        // we mutate BEFORE enqueueing and refresh the trees immediately.
        auto assignProgress = [](std::vector<SyncItem>& items, std::vector<std::size_t> const& emitted) {
            for (auto idx : emitted)
            {
                auto prog = std::make_shared<Nui::Observed<double>>(0.0);
                items[idx].progress = prog;
                const std::string prefix = items[idx].relKey + "/";
                for (auto& other : items)
                {
                    if (other.relKey.size() > prefix.size() && other.relKey.starts_with(prefix))
                        other.progress = prog;
                }
            }
        };

        {
            auto uploads = uploadItems_.value();
            assignProgress(uploads, uploadIndices);
            uploadItems_ = std::move(uploads);
        }
        {
            auto downloads = downloadItems_.value();
            assignProgress(downloads, downloadIndices);
            downloadItems_ = std::move(downloads);
        }
        {
            auto deletes = deleteItems_.value();
            assignProgress(deletes, deleteIndices);
            deleteItems_ = std::move(deletes);
        }

        refreshTrees();
        Nui::globalEventContext.executeActiveEventsImmediately();

        auto hookProgress = [this](std::shared_ptr<Nui::Observed<double>> prog)
        {
            return [this, prog](std::optional<Ids::OperationId> const& opId, std::string const&)
            {
                if (!opId)
                {
                    *prog = -1.0;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    return;
                }
                operationQueue_->addTransferProgressCallback(
                    *opId,
                    [prog](double fraction)
                    {
                        *prog = fraction;
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                );
                operationQueue_->addCompletionCallback(
                    *opId,
                    [prog](bool /*success*/)
                    {
                        *prog = 1.1;
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    }
                );
            };
        };

        // See enqueueSingle: in non-recursive mode missing directories must be
        // created via createDirectory (not bulk-transferred).
        const bool nonRecursive = !recursive_.value();
        auto onDirCreatedFor = [](std::shared_ptr<Nui::Observed<double>> prog) {
            return [prog](bool success, std::string const&) {
                *prog = success ? 1.1 : -1.0;
                Nui::globalEventContext.executeActiveEventsImmediately();
            };
        };

        auto const& uploadsSnap = uploadItems_.value();
        for (auto idx : uploadIndices)
        {
            auto const& itm = uploadsSnap[idx];
            if (nonRecursive && isDirectoryItem(itm))
            {
                operationQueue_->createRemoteDirectory(
                    remotePath_ / itm.relKey, onDirCreatedFor(itm.progress)
                );
            }
            else if (itm.localItem && itm.remoteItem)
            {
                operationQueue_->enqueueUpload(
                    *itm.remoteItem, *itm.localItem, hookProgress(itm.progress), true, true,
                    /*createMissingDirs=*/true
                );
            }
            else if (itm.localItem)
            {
                SharedData::DirectoryEntry remoteStub = *itm.localItem;
                remoteStub.path = itm.localItem->path;
                remoteStub.fullPath = remotePath_ / itm.localItem->path;
                operationQueue_->enqueueUpload(
                    NuiFileExplorer::Item{remoteStub}, *itm.localItem, hookProgress(itm.progress), true, true,
                    /*createMissingDirs=*/true
                );
            }
        }

        auto const& downloadsSnap = downloadItems_.value();
        for (auto idx : downloadIndices)
        {
            auto const& itm = downloadsSnap[idx];
            if (nonRecursive && isDirectoryItem(itm))
            {
                operationQueue_->createLocalDirectory(
                    localPath_ / itm.relKey, onDirCreatedFor(itm.progress)
                );
            }
            else if (itm.localItem && itm.remoteItem)
            {
                operationQueue_->enqueueDownload(
                    *itm.remoteItem, *itm.localItem, hookProgress(itm.progress), true, true,
                    /*createMissingDirs=*/true
                );
            }
            else if (itm.remoteItem)
            {
                SharedData::DirectoryEntry localStub = *itm.remoteItem;
                localStub.path = itm.remoteItem->path;
                localStub.fullPath = localPath_ / itm.remoteItem->path;
                operationQueue_->enqueueDownload(
                    *itm.remoteItem, NuiFileExplorer::Item{localStub}, hookProgress(itm.progress), true, true,
                    /*createMissingDirs=*/true
                );
            }
        }

        auto const& deletesSnap = deleteItems_.value();
        std::vector<std::filesystem::path> delPaths;
        delPaths.reserve(deleteIndices.size());
        for (auto idx : deleteIndices)
        {
            auto const& itm = deletesSnap[idx];
            if (itm.action == SyncItemAction::DeleteRemote && itm.remoteItem)
                delPaths.push_back(itm.remoteItem->fullPath);
            else if (itm.action == SyncItemAction::DeleteLocal && itm.localItem)
                delPaths.push_back(itm.localItem->fullPath);
        }
        if (!delPaths.empty())
            operationQueue_->enqueueDelete(delPaths, recursive_.value(), [](auto const&, auto const&) {});
    }
};

// ---- Tree integration ------------------------------------------------------

namespace
{
    /** @brief Pull a SyncItem out of a Tree row's userData (pointer type is
     *         expected, may be null — callers must check).
     */
    SyncItem const* userDataAsSyncItem(std::any const& userData)
    {
        auto const* ptr = std::any_cast<SyncItem const*>(&userData);
        return ptr ? *ptr : nullptr;
    }
}

ScriptNuiComponents::Tree::RowContentRenderer
SyncDialog::Implementation::makeTreeRowRenderer(Nui::Observed<std::vector<SyncItem>>& listObs, bool mirrored)
{
    // Where do content cells live for the current list?  Static for Upload /
    // Download (driven by the tree's `mirrored` flag), but the Delete list is
    // one-sided and switches sides with the current direction: DeleteRemote
    // → right, DeleteLocal → left.  Synthetic directory rows read this at
    // render time so their labels track the leaves underneath them.
    auto contentOnRight = [mirrored, &listObs]() -> bool {
        if (mirrored)
            return true;
        auto const& items = listObs.value();
        if (items.empty())
            return false;
        return items.front().action == SyncItemAction::DeleteRemote;
    };
    return [this, &listObs, contentOnRight](ScriptNuiComponents::Tree::RowContext const& ctx) -> Nui::ElementRenderer {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;
        using Nui::Elements::span;

        SyncItem const* itemPtr = userDataAsSyncItem(ctx.userData);
        if (!itemPtr)
        {
            // Directory row — the tree provides the chevron + caller fills in a
            // label based on the directory's basename derived from the NodeId.
            const auto& fullKey = ctx.id;
            std::string_view view{fullKey};
            if (!view.empty() && view.back() == '/')
                view.remove_suffix(1);
            const auto slash = view.rfind('/');
            const auto basename = (slash == std::string_view::npos) ? view : view.substr(slash + 1);
            // Place the folder label on the same side as the leaf content in
            // this list — otherwise a parent directory ends up floating on
            // the opposite side of the row from its own children.
            const bool labelRight = contentOnRight();
            auto labelCell = span{
                class_ = labelRight
                    ? "sync-diff-cell sync-diff-cell-right sync-diff-cell--directory"
                    : "sync-diff-cell sync-diff-cell--directory"
            }(span{class_ = "name"}(std::string{basename}));
            auto emptyCell = span{class_ = "sync-diff-cell sync-diff-cell--directory"}();
            if (labelRight)
            {
                return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                    std::move(emptyCell),
                    div{class_ = "sync-diff-arrow"}(),
                    std::move(labelCell)
                );
            }
            return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                std::move(labelCell),
                div{class_ = "sync-diff-arrow"}(),
                std::move(emptyCell)
            );
        }

        SyncItem const& itm = *itemPtr;
        std::string arrowClass;
        Nui::ElementRenderer arrowIcon = Nui::nil();
        switch (itm.action)
        {
            case SyncItemAction::Upload:
                arrowClass = "upload";
                arrowIcon = Ui5Icons::arrow_right();
                break;
            case SyncItemAction::Download:
                arrowClass = "download";
                arrowIcon = Ui5Icons::arrow_left();
                break;
            case SyncItemAction::DeleteLocal:
            case SyncItemAction::DeleteRemote:
                arrowClass = "delete";
                arrowIcon = Ui5Icons::delete_();
                break;
        }

        const std::string relKeyCopy = itm.relKey;

        // Single click target: the centre arrow / trashcan icon.  Hover
        // brightens and a press scales it down — see `sync_dialog.css`.
        auto makeArrow = [&]() {
            return div{
                class_ = fmt::format("sync-diff-arrow sync-diff-arrow--clickable {}", arrowClass),
                "title"_attr = language->get("syncDialog", "syncItemNowTitle"),
                onClick = [this, &listObs, relKeyCopy](Nui::val event) {
                    event.call<void>("stopPropagation");
                    enqueueSingleByRelKey(listObs, relKeyCopy);
                },
            }(std::move(arrowIcon));
        };

        // Leaves render on the side that carries the data: local→left,
        // remote→right.  DeleteLocal/DeleteRemote inherit this via their
        // populated side, so the Delete list naturally switches sides based
        // on the current direction.
        // Progress overlay is painted by `.sync-diff-row::before` via the
        // `--sync-row-bg` CSS custom property set here.  Keeping it on
        // `.sync-diff-row`'s own `style` (not the outer tree row) avoids
        // clobbering the tree's `--depth` var.
        if (itm.progress)
        {
            auto prog = itm.progress;
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
                renderItemCell(itm.localItem, false),
                makeArrow(),
                renderItemCell(itm.remoteItem, true)
            );
        }

        return div{class_ = "sync-diff-row"}(
            renderItemCell(itm.localItem, false),
            makeArrow(),
            renderItemCell(itm.remoteItem, true)
        );
    };
}

ScriptNuiComponents::Tree::RowAttributeProvider SyncDialog::Implementation::makeTreeRowAttributes()
{
    // Unused for now.  Kept as a seam in case we later want to add per-row
    // classes / data attrs on the outer tree row.  The progress background is
    // rendered via a `::before` pseudo-element on `.sync-diff-row` driven by a
    // local CSS variable (see `sync_dialog.css`), so it stays off the tree
    // row's inline style and does not clobber the tree's `--depth` variable.
    return {};
}

namespace
{
    /** @brief Returns the set of tree-leaf relKeys — items in @p list that have
     *         no other item whose relKey is a strict descendant path.  The tree
     *         stores selection on leaf NodeIds only; directories derive their
     *         tristate from descendants.
     */
    std::unordered_set<std::string> collectLeafRelKeys(std::vector<SyncItem> const& list)
    {
        std::unordered_set<std::string> leaves;
        leaves.reserve(list.size());
        for (auto const& item : list)
        {
            const std::string prefix = item.relKey + "/";
            const bool hasChild = std::any_of(list.begin(), list.end(), [&](SyncItem const& other) {
                return other.relKey.size() > prefix.size() && other.relKey.starts_with(prefix);
            });
            if (!hasChild)
                leaves.insert(item.relKey);
        }
        return leaves;
    }
}

void SyncDialog::Implementation::resetSelectionAllChecked()
{
    uploadSelected_->value() = collectLeafRelKeys(uploadItems_.value());
    uploadSelected_->modify();
    downloadSelected_->value() = collectLeafRelKeys(downloadItems_.value());
    downloadSelected_->modify();
    deleteSelected_->value() = collectLeafRelKeys(deleteItems_.value());
    deleteSelected_->modify();
}

void SyncDialog::Implementation::refreshTrees()
{
    namespace Snc = ScriptNuiComponents;

    auto fold = [](std::vector<SyncItem> const& items) {
        return Snc::foldByRelKey<SyncItem>(
            items,
            [](SyncItem const& item) -> std::string_view { return item.relKey; },
            [](SyncItem const& item) -> std::any { return static_cast<SyncItem const*>(&item); });
    };

    // The fold captures pointers into the Observed's underlying vector; Observed
    // stores its value in-place and the pointers stay valid for as long as the
    // vector isn't reassigned.  setRoots() consumes the nodes synchronously so
    // the pointers only need to survive until the tree finishes its keyed merge.
    uploadTree_.setRoots(fold(uploadItems_.value()));
    downloadTree_.setRoots(fold(downloadItems_.value()));
    deleteTree_.setRoots(fold(deleteItems_.value()));
}

void SyncDialog::Implementation::enqueueSingleByRelKey(
    Nui::Observed<std::vector<SyncItem>>& list, std::string const& relKey)
{
    auto const& items = list.value();
    std::size_t targetIdx = items.size();
    for (std::size_t idx = 0; idx < items.size(); ++idx)
    {
        if (items[idx].relKey == relKey)
        {
            targetIdx = idx;
            break;
        }
    }
    if (targetIdx == items.size())
        return;

    enqueueSingle(list, targetIdx);

    // Backend always transfers the whole subtree for a directory-level
    // operation, but only the triggering SyncItem gets a progress observer
    // from enqueueSingle.  Share it with every descendant SyncItem (by
    // relKey prefix) so their rows reflect the same gradient/done/error
    // state instead of staying indefinitely "pending".
    auto updated = list.value();
    if (targetIdx >= updated.size())
        return;
    auto targetProgress = updated[targetIdx].progress;
    if (!targetProgress)
        return;
    const std::string prefix = relKey + "/";
    bool anyChanged = false;
    for (auto& item : updated)
    {
        if (item.relKey.size() > prefix.size() && item.relKey.starts_with(prefix))
        {
            item.progress = targetProgress;
            anyChanged = true;
        }
    }
    if (anyChanged)
    {
        list = std::move(updated);
        refreshTrees();
        Nui::globalEventContext.executeActiveEventsImmediately();
    }
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

void SyncDialog::setOnRecompare(
    std::function<void(
        std::filesystem::path,
        std::filesystem::path,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(
            std::vector<SharedData::DirectoryEntry>,
            std::vector<SharedData::DirectoryEntry>
        )>
    )> callback
)
{
    impl_->onRecompare_ = std::move(callback);
}

void SyncDialog::open(
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    std::vector<SharedData::DirectoryEntry> localEntries,
    std::vector<SharedData::DirectoryEntry> remoteEntries
)
{
    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);
    impl_->localEntries_ = std::move(localEntries);
    impl_->remoteEntries_ = std::move(remoteEntries);

    impl_->uploadCollapsed_ = false;
    impl_->downloadCollapsed_ = false;
    impl_->deleteCollapsed_ = false;

    impl_->recomputeDiff();

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

    // Localized labels for the direction select.  Strings are also used as
    // the comparison values when mapping back to the SyncDirection enum; a
    // mid-dialog language switch will desync directionStr_ until reopen.
    const std::vector<std::string> directionOptions{
        language->get("syncDialog", "directionBoth"),
        language->get("syncDialog", "directionUploadOnly"),
        language->get("syncDialog", "directionDownloadOnly"),
    };
    const std::string directionUploadOnly = directionOptions[1];
    const std::string directionDownloadOnly = directionOptions[2];

    auto onSettingChange = [this]()
    {
        impl_->recomputeDiff();
        Nui::globalEventContext.executeActiveEventsImmediately();
    };

    // Row content is now rendered inside the tree — see
    // `Implementation::makeTreeRowRenderer`.  The 4-cell grid layout lives in
    // `.sync-diff-row` and is unchanged; the tree wraps it with indent +
    // chevron outside.

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
            // Backdrop click minimizes (preserves state) instead of closing.
            // The explicit X button in the dialog header closes/resets.
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
            // ----------------------------------------------------------------
            // Header
            // ----------------------------------------------------------------
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

            // ----------------------------------------------------------------
            // Body
            // ----------------------------------------------------------------
            div{class_ = "sync-dialog-body"}(
                // Settings panel — groups rendered as cards
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
                    // ---- Upload section ----
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
                                observe(impl_->uploadItems_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    const auto totals = computeTotals(impl_->uploadItems_.value());
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "uploadSectionCount")),
                                        totals.count,
                                        Utility::formatBytes(static_cast<long long>(totals.bytes))));
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

                    // ---- Download section ----
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
                                observe(impl_->downloadItems_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    const auto totals = computeTotals(impl_->downloadItems_.value());
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "downloadSectionCount")),
                                        totals.count,
                                        Utility::formatBytes(static_cast<long long>(totals.bytes))));
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

                    // ---- Delete section ----
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
                                observe(impl_->deleteItems_),
                                [this]() -> Nui::ElementRenderer {
                                    using Nui::Elements::span;
                                    const auto totals = computeTotals(impl_->deleteItems_.value());
                                    return span{}(fmt::format(
                                        fmt::runtime(language->get("syncDialog", "deleteSectionCount")),
                                        totals.count,
                                        Utility::formatBytes(static_cast<long long>(totals.bytes))));
                                }
                            )
                        ),
                        div{
                            // The Delete tree is one-sided and switches sides
                            // with the current direction (DeleteRemote → right,
                            // DeleteLocal → left).  Tree::Options::mirror is
                            // static, so we layer the mirror class on this
                            // wrapper instead — CSS uses descendant selectors
                            // (`.script-nui-tree--mirrored .script-nui-tree__row`
                            // etc.) so the same styling kicks in either way.
                            class_ = observe(impl_->deleteItems_).generate([this]() -> std::string {
                                auto const& items = impl_->deleteItems_.value();
                                const bool mirror = !items.empty()
                                    && items.front().action == SyncItemAction::DeleteRemote;
                                return mirror
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

            // ----------------------------------------------------------------
            // Footer
            // ----------------------------------------------------------------
            div{class_ = "sync-dialog-footer"}(
                div{class_ = "sync-footer-actions"}(
                Snc::button({
                    .text = language->getObserved("syncDialog", "recompare"),
                    .icon = Ui5Icons::refresh(),
                    .attributes = {
                        onClick = [this](Nui::val) {
                            if (impl_->onRecompare_)
                            {
                                impl_->onRecompare_(
                                    impl_->localPath_,
                                    impl_->remotePath_,
                                    impl_->respectIgnore_.value(),
                                    impl_->recursive_.value(),
                                    impl_->ignoreHidden_.value(),
                                    [this](auto localE, auto remoteE) {
                                        open(
                                            impl_->localPath_,
                                            impl_->remotePath_,
                                            std::move(localE),
                                            std::move(remoteE)
                                        );
                                    }
                                );
                            }
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
                        impl_->uploadItems_, impl_->downloadItems_, impl_->deleteItems_
                    ),
                    [this]() -> Nui::ElementRenderer {
                        using Nui::Elements::span;
                        const auto up = computeTotals(impl_->uploadItems_.value(), &impl_->uploadSelected_->value());
                        const auto down = computeTotals(impl_->downloadItems_.value(), &impl_->downloadSelected_->value());
                        const auto del = computeTotals(impl_->deleteItems_.value(), &impl_->deleteSelected_->value());
                        const std::size_t total = up.count + down.count + del.count;
                        const std::uint64_t bytes = up.bytes + down.bytes + del.bytes;
                        return span{}(fmt::format(
                            fmt::runtime(language->get("syncDialog", "footerSummary")),
                            total,
                            Utility::formatBytes(static_cast<long long>(bytes))
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
                )   // sync-footer-actions
            )       // sync-dialog-footer
        )           // sync-dialog
    );              // sync-dialog-blocker
    // clang-format on
}
