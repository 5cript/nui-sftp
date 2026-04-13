#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/sync_item.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>

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
#include <string>
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
        const auto name = entry.path.generic_string();

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
    Nui::Observed<bool> actionUpload_{true};
    Nui::Observed<bool> actionDownload_{true};
    Nui::Observed<bool> actionDelete_{false};

    // Diff item lists
    Nui::Observed<std::vector<SyncItem>> uploadItems_{};
    Nui::Observed<std::vector<SyncItem>> downloadItems_{};
    Nui::Observed<std::vector<SyncItem>> deleteItems_{};

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
        initTrees();
    }

    void initTrees()
    {
        namespace Snc = ScriptNuiComponents;
        uploadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(uploadItems_),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = false,
            .showIcons = false,
        }};
        downloadTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(downloadItems_),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = false,
            .showIcons = false,
            .mirror = true,
        }};
        deleteTree_ = Snc::Tree{Snc::Tree::Options{
            .rowContent = makeTreeRowRenderer(deleteItems_),
            .rowAttributes = makeTreeRowAttributes(),
            .showCheckboxes = false,
            .showIcons = false,
        }};
    }

    ScriptNuiComponents::Tree::RowContentRenderer
    makeTreeRowRenderer(Nui::Observed<std::vector<SyncItem>>& listObs);

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
                    deletes.push_back(makeItem(
                        SyncItemAction::DeleteLocal, makeLocalItem(localEntry, relKey), std::nullopt, relKey));
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
                deletes.push_back(
                    makeItem(SyncItemAction::DeleteRemote, std::nullopt, makeRemoteItem(remoteEntry, relKey), relKey));
        }

        uploadItems_ = std::move(uploads);
        downloadItems_ = std::move(downloads);
        deleteItems_ = std::move(deletes);
        refreshTrees();
    }

    /** @brief Enqueues a single item from one of the three diff lists at priority.
     *
     * @param list  The observed list the item belongs to (upload, download or delete).
     * @param index Index of the item inside @p list.
     */
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

        switch (itemCopy.action)
        {
            case SyncItemAction::Upload:
            {
                if (itemCopy.localItem && itemCopy.remoteItem)
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
                if (itemCopy.localItem && itemCopy.remoteItem)
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

    void enqueueOperations()
    {
        // Assign progress observers to every item before enqueueing so the DOM
        // rows are rendered with observe(*itm.progress) before any callbacks fire.
        auto uploads = uploadItems_.value();
        for (auto& itm : uploads)
            itm.progress = std::make_shared<Nui::Observed<double>>(0.0);
        uploadItems_ = std::move(uploads);

        auto downloads = downloadItems_.value();
        for (auto& itm : downloads)
            itm.progress = std::make_shared<Nui::Observed<double>>(0.0);
        downloadItems_ = std::move(downloads);

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

        for (auto const& itm : uploadItems_.value())
        {
            if (itm.localItem && itm.remoteItem)
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

        for (auto const& itm : downloadItems_.value())
        {
            if (itm.localItem && itm.remoteItem)
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

        std::vector<std::filesystem::path> delPaths;
        for (auto const& itm : deleteItems_.value())
        {
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
SyncDialog::Implementation::makeTreeRowRenderer(Nui::Observed<std::vector<SyncItem>>& listObs)
{
    return [this, &listObs](ScriptNuiComponents::Tree::RowContext const& ctx) -> Nui::ElementRenderer {
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
            return div{class_ = "sync-diff-row sync-diff-row--directory"}(
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}(
                    span{class_ = "name"}(std::string{basename})),
                div{class_ = "sync-diff-arrow"}(),
                span{class_ = "sync-diff-cell sync-diff-cell--directory"}()
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
                "title"_attr = std::string{"Sync this item now"},
                onClick = [this, &listObs, relKeyCopy](Nui::val event) {
                    event.call<void>("stopPropagation");
                    enqueueSingleByRelKey(listObs, relKeyCopy);
                },
            }(std::move(arrowIcon));
        };

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

    static const std::vector<std::string> directionOptions{"Both"s, "Upload only"s, "Download only"s};

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
        style = observe(impl_->open_).generate([this]() {
            return impl_->open_.value() ? "display: flex;"s : "display: none;"s;
        }),
        onClick = [this](Nui::val event) {
            event.call<void>("stopPropagation");
            impl_->open_ = false;
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
                            "Synchronize: {} / {}",
                            impl_->localPath_.filename().string(),
                            impl_->remotePath_.filename().string()
                        ));
                    }
                ),
                Snc::button({
                    .icon = GeneratedSvgs::decline(),
                    .attributes = {
                        onClick = [this]() {
                            impl_->open_ = false;
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
                        label{class_ = "sync-settings-label"}("Direction"),
                        Snc::select(Snc::SelectOptions<decltype(impl_->directionStr_), std::vector<std::string>>{
                            .activeOption = impl_->directionStr_,
                            .options = directionOptions,
                            .attributes = {style = "min-width: 160px;"},
                            .onChange = [this, onSettingChange](std::string const& val, Nui::WebApi::MouseEvent const&) {
                                impl_->directionStr_ = val;
                                if (val == "Upload only"s)
                                    impl_->direction_ = SyncDirection::Upload;
                                else if (val == "Download only"s)
                                    impl_->direction_ = SyncDirection::Download;
                                else
                                    impl_->direction_ = SyncDirection::Both;
                                onSettingChange();
                            }
                        })
                    ),
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}("Options"),
                        div{class_ = "sync-settings-switches"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->recursive_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->recursive_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Recursive")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->respectIgnore_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->respectIgnore_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Respect .ignore / .gitignore")
                            )
                        )
                    ),
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}("Actions"),
                        div{class_ = "sync-settings-switches sync-settings-switches-2col"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionUpload_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionUpload_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Upload")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDownload_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionDownload_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Download")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDelete_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->actionDelete_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Delete")
                            )
                        )
                    )
                ),

                // Column headers
                div{class_ = "sync-diff-header"}(
                    span{class_ = "sync-diff-col-label"}("Local"),
                    span{}(),
                    span{class_ = "sync-diff-col-label sync-diff-col-label-right"}("Remote"),
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
                                    return span{}(fmt::format("Upload ({})", impl_->uploadItems_.value().size()));
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
                                    return span{}(fmt::format("Download ({})", impl_->downloadItems_.value().size()));
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
                                    return span{}(fmt::format("Delete ({})", impl_->deleteItems_.value().size()));
                                }
                            )
                        ),
                        div{
                            class_ = "sync-diff-section-rows",
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
                    .text = "Recompare"s,
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
                                .text = "Resume Queue"s,
                                .icon = Ui5Icons::play(),
                                .attributes = {
                                    onClick = [this](Nui::val) { impl_->operationQueue_->unpause(); }
                                },
                                .styleVariant = Snc::StyleVariant::Success,
                            });
                        }
                        return div{class_ = "sync-queue-running-indicator"}(
                            div{class_ = "sync-queue-running-dot"}(),
                            span{}("Queue running")
                        );
                    }
                ),
                Snc::button({
                    .text = "Synchronize"s,
                    .icon = Ui5Icons::synchronize(),
                    .attributes = {
                        onClick = [this](Nui::val) {
                            impl_->confirmDialog_->open({
                                .styleVariant = Snc::StyleVariant::Warning,
                                .headerText = "Synchronize Directories",
                                .text = fmt::format(
                                    "Synchronize {} with {}?",
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
