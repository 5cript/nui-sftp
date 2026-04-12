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

    /** @brief Returns true when the two entries are considered different under the given criteria.
     *
     * Symlinks are compared by raw link target (readlink value): two links with the same target
     * are in sync, two with different targets are not — size and mtime of the link itself are
     * meaningless for comparison. A type mismatch (symlink vs regular file) always counts as a diff.
     */
    bool entriesDiffer(
        SharedData::DirectoryEntry const& loc,
        SharedData::DirectoryEntry const& rem,
        bool criteriaSize,
        bool criteriaMtime
    )
    {
        if (loc.type != rem.type)
            return true;
        if (loc.type == SharedData::FileType::Symlink)
        {
            if (loc.linkTarget && rem.linkTarget)
                return *loc.linkTarget != *rem.linkTarget;
            // If either side is missing the link target, fall back to considering them equal so
            // we don't churn re-uploading links we can't compare.
            return false;
        }
        if (criteriaSize && loc.size != rem.size)
            return true;
        if (criteriaMtime && loc.mtime != rem.mtime)
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
    Nui::Observed<bool> criteriaName_{true};
    Nui::Observed<bool> criteriaSize_{true};
    Nui::Observed<bool> criteriaMtime_{true};
    Nui::Observed<bool> criteriaHash_{false};
    Nui::Observed<bool> recursive_{true};
    Nui::Observed<bool> actionUpload_{true};
    Nui::Observed<bool> actionDownload_{true};
    Nui::Observed<bool> actionDelete_{false};

    // Diff item lists
    Nui::Observed<std::vector<SyncItem>> uploadItems_{};
    Nui::Observed<std::vector<SyncItem>> downloadItems_{};
    Nui::Observed<std::vector<SyncItem>> deleteItems_{};

    Nui::Observed<bool> uploadCollapsed_{false};
    Nui::Observed<bool> downloadCollapsed_{false};
    Nui::Observed<bool> deleteCollapsed_{false};

    ConfirmDialog* confirmDialog_;
    OperationQueue* operationQueue_;
    std::function<void(
        std::filesystem::path,
        std::filesystem::path,
        std::function<void(
            std::vector<SharedData::DirectoryEntry>,
            std::vector<SharedData::DirectoryEntry>
        )>
    )>
        onRecompare_{};

    explicit Implementation(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
        : confirmDialog_{confirmDialog}
        , operationQueue_{operationQueue}
    {}

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
            return;
        }

        const bool sizeCheck = criteriaSize_.value();
        const bool mtimeCheck = criteriaMtime_.value();

        auto localMap = buildEntryMap(localPath_, localEntries_);
        auto remoteMap = buildEntryMap(remotePath_, remoteEntries_);

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

        // --- Entries that exist locally ---
        for (auto const& [relKey, localEntry] : localMap)
        {
            auto remIt = remoteMap.find(relKey);
            if (remIt == remoteMap.end())
            {
                if (actionUpload_.value() && direction_ != SyncDirection::Download)
                    uploads.push_back({SyncItemAction::Upload, makeLocalItem(localEntry, relKey), std::nullopt});
                else if (actionDelete_.value() && direction_ == SyncDirection::Download)
                    deletes.push_back({SyncItemAction::DeleteLocal, makeLocalItem(localEntry, relKey), std::nullopt});
            }
            else
            {
                const auto& remoteEntry = remIt->second;
                if (localEntry.type == SharedData::FileType::Directory)
                    continue;

                if (!entriesDiffer(localEntry, remoteEntry, sizeCheck, mtimeCheck))
                    continue;

                const bool localNewer = localEntry.mtime >= remoteEntry.mtime;

                if (direction_ == SyncDirection::Upload ||
                    (direction_ == SyncDirection::Both && localNewer))
                {
                    if (actionUpload_.value())
                        uploads.push_back({
                            SyncItemAction::Upload,
                            makeLocalItem(localEntry, relKey),
                            makeRemoteItem(remoteEntry, relKey),
                        });
                }
                else
                {
                    if (actionDownload_.value())
                        downloads.push_back({
                            SyncItemAction::Download,
                            makeLocalItem(localEntry, relKey),
                            makeRemoteItem(remoteEntry, relKey),
                        });
                }
            }
        }

        // --- Entries that exist only remotely ---
        for (auto const& [relKey, remoteEntry] : remoteMap)
        {
            if (localMap.count(relKey))
                continue;

            if (actionDownload_.value() && direction_ != SyncDirection::Upload)
                downloads.push_back({SyncItemAction::Download, std::nullopt, makeRemoteItem(remoteEntry, relKey)});
            else if (actionDelete_.value() && direction_ == SyncDirection::Upload)
                deletes.push_back({SyncItemAction::DeleteRemote, std::nullopt, makeRemoteItem(remoteEntry, relKey)});
        }

        uploadItems_ = std::move(uploads);
        downloadItems_ = std::move(downloads);
        deleteItems_ = std::move(deletes);
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
                    *itm.remoteItem, *itm.localItem, hookProgress(itm.progress), true, true
                );
            }
            else if (itm.localItem)
            {
                SharedData::DirectoryEntry remoteStub = *itm.localItem;
                remoteStub.path = itm.localItem->path;
                remoteStub.fullPath = remotePath_ / itm.localItem->path;
                operationQueue_->enqueueUpload(
                    NuiFileExplorer::Item{remoteStub}, *itm.localItem, hookProgress(itm.progress), true, true
                );
            }
        }

        for (auto const& itm : downloadItems_.value())
        {
            if (itm.localItem && itm.remoteItem)
            {
                operationQueue_->enqueueDownload(
                    *itm.remoteItem, *itm.localItem, hookProgress(itm.progress), true, true
                );
            }
            else if (itm.remoteItem)
            {
                SharedData::DirectoryEntry localStub = *itm.remoteItem;
                localStub.path = itm.remoteItem->path;
                localStub.fullPath = localPath_ / itm.remoteItem->path;
                operationQueue_->enqueueDownload(
                    *itm.remoteItem, NuiFileExplorer::Item{localStub}, hookProgress(itm.progress), true, true
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

    // Renders a single diff row, with a reactive progress/done background if progress is set.
    auto renderRow = [](long long /*idx*/, SyncItem const& itm) -> Nui::ElementRenderer
    {
        using namespace Nui::Elements;
        using namespace Nui::Attributes;
        using Nui::Elements::div;

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

        if (itm.progress)
        {
            auto prog = itm.progress;
            return div{
                class_ = "sync-diff-row",
                style = observe(*prog).generate([prog]() -> std::string
                {
                    const double val = prog->value();
                    if (val > 1.0)
                        return "background: var(--sync-done-color, rgba(76,175,80,0.18));";
                    if (val < 0.0)
                        return "background: var(--sync-error-color, rgba(231,76,60,0.18));";
                    const int pct = static_cast<int>(val * 100.0);
                    return fmt::format(
                        "background: linear-gradient(to right,"
                        " var(--sync-progress-color, rgba(76,175,80,0.25)) {}%,"
                        " transparent {}%);",
                        pct, pct
                    );
                })
            }(
                renderItemCell(itm.localItem, false),
                div{class_ = fmt::format("sync-diff-arrow {}", arrowClass)}(std::move(arrowIcon)),
                renderItemCell(itm.remoteItem, true)
            );
        }

        return div{class_ = "sync-diff-row"}(
            renderItemCell(itm.localItem, false),
            div{class_ = fmt::format("sync-diff-arrow {}", arrowClass)}(std::move(arrowIcon)),
            renderItemCell(itm.remoteItem, true)
        );
    };

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
                        label{class_ = "sync-settings-label"}("Match by"),
                        div{class_ = "sync-settings-switches sync-settings-switches-2col"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaName_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->criteriaName_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Name")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaSize_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->criteriaSize_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Size")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaMtime_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->criteriaMtime_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Time")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaHash_,
                                    .onChange = [this, onSettingChange](bool val, auto const&) {
                                        impl_->criteriaHash_ = val; onSettingChange();
                                    }
                                }),
                                span{}("Hash")
                            )
                        )
                    ),
                    div{class_ = "sync-settings-card"}(
                        label{class_ = "sync-settings-label"}("Options"),
                        div{class_ = "sync-settings-switches"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->recursive_,
                                    .onChange = [this](bool val, auto const&) { impl_->recursive_ = val; }
                                }),
                                span{}("Recursive")
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
                    span{class_ = "sync-diff-col-label sync-diff-col-label-right"}("Remote")
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
                            Nui::range(impl_->uploadItems_),
                            renderRow
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
                            Nui::range(impl_->downloadItems_),
                            renderRow
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
                            Nui::range(impl_->deleteItems_),
                            renderRow
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
