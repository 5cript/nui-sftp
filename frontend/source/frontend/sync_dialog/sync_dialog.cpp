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

#include <shared_data/directory_entry.hpp>
#include <shared_data/sync_phase.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <chrono>
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
        const auto name = entry.path.filename().string();

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
                return span{class_ = "meta"}(mtimeStr);
            }()
        );
    }

    Nui::ElementRenderer renderSyncRow(long long /*index*/, SyncItem const& itm)
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

        return div{
            class_ = "sync-diff-row"
        }(renderItemCell(itm.localItem, false),
            div{class_ = fmt::format("sync-diff-arrow {}", arrowClass)}(std::move(arrowIcon)),
            renderItemCell(itm.remoteItem, true));
    }
}

// ---- Implementation ---------------------------------------------------------

struct SyncDialog::Implementation
{
    Nui::Observed<bool> open_{false};
    std::filesystem::path localPath_{};
    std::filesystem::path remotePath_{};

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

    // Diff item lists — one per action type for independent collapse/reactivity
    Nui::Observed<std::vector<SyncItem>> uploadItems_{};
    Nui::Observed<std::vector<SyncItem>> downloadItems_{};
    Nui::Observed<std::vector<SyncItem>> deleteItems_{};

    Nui::Observed<bool> uploadCollapsed_{false};
    Nui::Observed<bool> downloadCollapsed_{false};
    Nui::Observed<bool> deleteCollapsed_{false};

    ConfirmDialog* confirmDialog_;
    OperationQueue* operationQueue_;
    std::function<void(std::filesystem::path, std::filesystem::path, std::function<void()>)> onRecompare_{};

    explicit Implementation(ConfirmDialog* confirmDialog, OperationQueue* operationQueue)
        : confirmDialog_{confirmDialog}
        , operationQueue_{operationQueue}
    {}

    void enqueueOperations()
    {
        for (auto const& itm : uploadItems_.value())
        {
            if (itm.localItem && itm.remoteItem)
            {
                operationQueue_->enqueueUpload(
                    *itm.remoteItem, *itm.localItem, [](auto const&, auto const&) {}, true, true
                );
            }
        }
        for (auto const& itm : downloadItems_.value())
        {
            if (itm.localItem && itm.remoteItem)
            {
                operationQueue_->enqueueDownload(
                    *itm.remoteItem, *itm.localItem, [](auto const&, auto const&) {}, true, true
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
        {
            operationQueue_->enqueueDelete(delPaths, recursive_.value(), [](auto const&, auto const&) {});
        }
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
    std::function<void(std::filesystem::path, std::filesystem::path, std::function<void()>)> callback)
{
    impl_->onRecompare_ = std::move(callback);
}

void SyncDialog::open(std::filesystem::path localPath, std::filesystem::path remotePath)
{
    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);

    impl_->uploadCollapsed_ = false;
    impl_->downloadCollapsed_ = false;
    impl_->deleteCollapsed_ = false;

    // Populate dummy items for UI testing (replaced by comparison logic later)
    auto makeItem = [](std::string name, SharedData::FileType ftype, std::uint64_t size, std::uint64_t mtime)
    {
        SharedData::DirectoryEntry entry;
        entry.path = name;
        entry.fullPath = name;
        entry.type = ftype;
        entry.size = size;
        entry.mtime = mtime;
        return NuiFileExplorer::Item{entry};
    };

    using FT = SharedData::FileType;
    // Upload: new local files (no remote counterpart) + locally-newer files (both sides present)
    impl_->uploadItems_ = std::vector<SyncItem>{
        {SyncItemAction::Upload, makeItem("notes.txt", FT::Regular, 4321, 1705000000), std::nullopt},
        {SyncItemAction::Upload, makeItem("image.png", FT::Regular, 123456, 1704900000), std::nullopt},
        {SyncItemAction::Upload, makeItem("assets", FT::Directory, 0, 1704800000), std::nullopt},
        // Both sides exist; local is newer
        {SyncItemAction::Upload,
            makeItem("config.toml", FT::Regular, 2048, 1705200000),
            makeItem("config.toml", FT::Regular, 2048, 1703000000)},
        {SyncItemAction::Upload,
            makeItem("build.sh", FT::Regular, 512, 1705300000),
            makeItem("build.sh", FT::Regular, 480, 1702000000)},
    };
    // Download: new remote files (no local counterpart) + remotely-newer files (both sides present)
    impl_->downloadItems_ = std::vector<SyncItem>{
        {SyncItemAction::Download, std::nullopt, makeItem("remote_config.json", FT::Regular, 8192, 1705100000)},
        {SyncItemAction::Download, std::nullopt, makeItem("server.log", FT::Regular, 524288, 1705050000)},
        // Both sides exist; remote is newer
        {SyncItemAction::Download,
            makeItem("README.md", FT::Regular, 1024, 1700000000),
            makeItem("README.md", FT::Regular, 1200, 1705400000)},
    };
    impl_->deleteItems_ = std::vector<SyncItem>{
        {SyncItemAction::DeleteLocal, makeItem("orphan.tmp", FT::Regular, 1024, 1700000000), std::nullopt},
        {SyncItemAction::DeleteRemote, std::nullopt, makeItem("stale_cache.bin", FT::Regular, 65536, 1698000000)},
    };

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
                    .icon = []() -> Nui::ElementRenderer {
                        return span{class_ = "sync-close-x"}("X");
                    }(),
                    .attributes = {
                        onClick = [this](Nui::val) {
                            impl_->open_ = false;
                            Nui::globalEventContext.executeActiveEventsImmediately();
                        }
                    }
                })
            ),

            // ----------------------------------------------------------------
            // Body
            // ----------------------------------------------------------------
            div{class_ = "sync-dialog-body"}(
                // Settings panel
                div{class_ = "sync-settings-panel"}(
                    div{class_ = "sync-settings-group"}(
                        label{class_ = "sync-settings-label"}("Direction"),
                        Snc::select(Snc::SelectOptions<decltype(impl_->directionStr_), std::vector<std::string>>{
                            .activeOption = impl_->directionStr_,
                            .options = directionOptions,
                            .attributes = {style = "min-width: 180px;"},
                            .onChange = [this](std::string const& val, Nui::WebApi::MouseEvent const&) {
                                impl_->directionStr_ = val;
                                if (val == "Upload only"s)
                                    impl_->direction_ = SyncDirection::Upload;
                                else if (val == "Download only"s)
                                    impl_->direction_ = SyncDirection::Download;
                                else
                                    impl_->direction_ = SyncDirection::Both;
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        })
                    ),
                    div{class_ = "sync-settings-group"}(
                        label{class_ = "sync-settings-label"}("Match by"),
                        div{class_ = "sync-settings-switches sync-settings-switches-2col"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaName_,
                                    .onChange = [this](bool val, auto const&) { impl_->criteriaName_ = val; }
                                }),
                                span{}("Name")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaSize_,
                                    .onChange = [this](bool val, auto const&) { impl_->criteriaSize_ = val; }
                                }),
                                span{}("Size")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaMtime_,
                                    .onChange = [this](bool val, auto const&) { impl_->criteriaMtime_ = val; }
                                }),
                                span{}("Time")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->criteriaHash_,
                                    .onChange = [this](bool val, auto const&) { impl_->criteriaHash_ = val; }
                                }),
                                span{}("Hash")
                            )
                        )
                    ),
                    div{class_ = "sync-settings-group"}(
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
                    div{class_ = "sync-settings-group"}(
                        label{class_ = "sync-settings-label"}("Shall"),
                        div{class_ = "sync-settings-switches sync-settings-switches-2col"}(
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionUpload_,
                                    .onChange = [this](bool val, auto const&) { impl_->actionUpload_ = val; }
                                }),
                                span{}("Upload")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDownload_,
                                    .onChange = [this](bool val, auto const&) { impl_->actionDownload_ = val; }
                                }),
                                span{}("Download")
                            ),
                            div{class_ = "sync-settings-switch-row"}(
                                Snc::switch_({
                                    .isChecked = impl_->actionDelete_,
                                    .onChange = [this](bool val, auto const&) { impl_->actionDelete_ = val; }
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
                            [](long long idx, SyncItem const& itm) -> Nui::ElementRenderer {
                                return renderSyncRow(idx, itm);
                            }
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
                            [](long long idx, SyncItem const& itm) -> Nui::ElementRenderer {
                                return renderSyncRow(idx, itm);
                            }
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
                            [](long long idx, SyncItem const& itm) -> Nui::ElementRenderer {
                                return renderSyncRow(idx, itm);
                            }
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
                                    [this]() { open(impl_->localPath_, impl_->remotePath_); }
                                );
                            }
                            else
                            {
                                open(impl_->localPath_, impl_->remotePath_);
                            }
                        }
                    }
                }),
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
