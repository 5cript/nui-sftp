#include <frontend/sync_dialog/sync_progress_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/components/progress_bar.hpp>
#include <frontend/svgs/decline.hpp>

#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/sync_scan_result.hpp>
#include <shared_data/sync_phase.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/utility/move_detector.hpp>

#include <ui5-sap-icons/icons/synchronize.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/style_variant.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SyncProgressDialog::Implementation
{
    Nui::Observed<bool> open_{false};
    Nui::Observed<SharedData::SyncPhase> phase_{SharedData::SyncPhase::Idle};

    // Listing counters (updated via scan progress callbacks)
    Nui::Observed<std::uint64_t> localListed_{0ull};
    Nui::Observed<std::uint64_t> remoteListed_{0ull};

    // Comparing counter — will be populated when the diff step is added
    Nui::Observed<std::uint64_t> compared_{0ull};

    // Hashing progress bar — total becomes known when comparing finishes
    Components::ProgressBar hashProgressBar_{Components::ProgressBar::Settings{
        .height = "10px",
        .min = 0,
        .max = 0,
        .showMinMax = true,
    }};

    std::filesystem::path localPath_{};
    std::filesystem::path remotePath_{};

    std::function<void(
        std::vector<SharedData::DirectoryEntry> localEntries,
        std::vector<SharedData::DirectoryEntry> remoteEntries
    )>
        onDone_{};

    // Intermediate storage while waiting for both scans to complete
    std::optional<std::vector<SharedData::DirectoryEntry>> localEntries_{};
    std::optional<std::vector<SharedData::DirectoryEntry>> remoteEntries_{};

    // Cancelled flag; replaced on each open() to invalidate stale captures
    std::shared_ptr<bool> cancelToken_{std::make_shared<bool>(false)};

    void checkBothComplete(SyncProgressDialog* /*dlg*/)
    {
        if (!localEntries_ || !remoteEntries_)
            return;

        // TODO: add Comparing phase here when hash/diff step is implemented
        phase_ = SharedData::SyncPhase::Done;
        open_ = false;
        Nui::globalEventContext.executeActiveEventsImmediately();

        if (onDone_)
            onDone_(std::move(*localEntries_), std::move(*remoteEntries_));
    }
};

SyncProgressDialog::SyncProgressDialog(OperationQueue* operationQueue)
    : operationQueue_{operationQueue}
    , impl_{std::make_unique<Implementation>()}
{}

SyncProgressDialog::~SyncProgressDialog() = default;
SyncProgressDialog::SyncProgressDialog(SyncProgressDialog&&) = default;
SyncProgressDialog& SyncProgressDialog::operator=(SyncProgressDialog&&) = default;

void SyncProgressDialog::open(
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    std::function<void(
        std::vector<SharedData::DirectoryEntry> localEntries,
        std::vector<SharedData::DirectoryEntry> remoteEntries
    )> onDone
)
{
    // Cancel any in-progress scan
    *impl_->cancelToken_ = true;
    auto token = std::make_shared<bool>(false);
    impl_->cancelToken_ = token;

    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);
    impl_->onDone_ = std::move(onDone);

    // Reset state
    impl_->phase_ = SharedData::SyncPhase::Listing;
    impl_->localListed_ = 0ull;
    impl_->remoteListed_ = 0ull;
    impl_->compared_ = 0ull;
    impl_->hashProgressBar_.max(0);
    impl_->localEntries_.reset();
    impl_->remoteEntries_.reset();
    impl_->open_ = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    operationQueue_->enqueueSyncScans(
        impl_->localPath_,
        impl_->remotePath_,
        // onRemoteProgress
        [this, token](SharedData::ScanProgress const& progress)
        {
            if (*token)
                return;
            impl_->remoteListed_ = progress.totalScanned;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        // onLocalProgress
        [this, token](SharedData::ScanProgress const& progress)
        {
            if (*token)
                return;
            impl_->localListed_ = progress.totalScanned;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        // onRemoteComplete
        [this, token](SharedData::SyncScanResult result)
        {
            if (*token)
                return;
            impl_->remoteEntries_ = std::move(result.entries);
            impl_->checkBothComplete(this);
        },
        // onLocalComplete
        [this, token](SharedData::SyncScanResult result)
        {
            if (*token)
                return;
            impl_->localEntries_ = std::move(result.entries);
            impl_->checkBothComplete(this);
        }
    );
}

void SyncProgressDialog::cancel()
{
    *impl_->cancelToken_ = true;
    impl_->cancelToken_ = std::make_shared<bool>(false);
    impl_->open_ = false;
    impl_->phase_ = SharedData::SyncPhase::Idle;
    impl_->localEntries_.reset();
    impl_->remoteEntries_.reset();
    Nui::globalEventContext.executeActiveEventsImmediately();
}

Nui::ElementRenderer SyncProgressDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using namespace Nui::Attributes::Literals;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    using namespace std::string_literals;
    // clang-format off
    return div{
        style = observe(impl_->open_).generate([this]() {
            return impl_->open_.value() ? "display: flex;"s : "display: none;"s;
        }),
        class_ = "sync-dialog-blocker sync-progress-dialog-blocker"
    }(
        div{class_ = "sync-dialog sync-progress-dialog"}(
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
                            "Comparing: {} / {}",
                            impl_->localPath_.filename().string(),
                            impl_->remotePath_.filename().string()
                        ));
                    }
                ),
                Snc::button({
                    .icon = GeneratedSvgs::decline(),
                    .attributes = {
                        onClick = [this]() { cancel(); }
                    },
                    .styleVariant = Snc::StyleVariant::Transparent,
                })
            ),

            // Body
            div{class_ = "sync-progress-body"}(
                observe(impl_->phase_),
                [this]() -> Nui::ElementRenderer {
                    using namespace Nui::Elements;
                    using namespace Nui::Attributes;
                    using Nui::Elements::div;
                    using Nui::Elements::span;

                    const auto phase = impl_->phase_.value();

                    if (phase == SharedData::SyncPhase::Listing)
                    {
                        return div{class_ = "sync-progress-step"}(
                            div{class_ = "sync-progress-step-header"}(
                                div{class_ = "sync-progress-spinner"}(),
                                span{class_ = "sync-progress-step-label"}("Listing files...")
                            ),
                            div{class_ = "sync-progress-counters"}(
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}("Local"),
                                    span{class_ = "sync-progress-counter-value"}(impl_->localListed_)
                                ),
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}("Remote"),
                                    span{class_ = "sync-progress-counter-value"}(impl_->remoteListed_)
                                )
                            )
                        );
                    }

                    // Comparing phase — placeholder for future diff/hash step
                    if (phase == SharedData::SyncPhase::Comparing)
                    {
                        return div{class_ = "sync-progress-step"}(
                            div{class_ = "sync-progress-step-header"}(
                                div{class_ = "sync-progress-spinner"}(),
                                span{class_ = "sync-progress-step-label"}("Comparing...")
                            ),
                            div{class_ = "sync-progress-counters"}(
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}("Differences found"),
                                    span{class_ = "sync-progress-counter-value"}(impl_->compared_)
                                )
                            )
                        );
                    }

                    // Hashing phase — placeholder for future hash-based comparison
                    if (phase == SharedData::SyncPhase::Hashing)
                    {
                        return div{class_ = "sync-progress-step"}(
                            div{class_ = "sync-progress-step-header"}(
                                span{class_ = "sync-progress-step-label"}("Hashing...")
                            ),
                            impl_->hashProgressBar_()
                        );
                    }

                    return div{}();
                }
            ),

            // Footer
            div{class_ = "sync-dialog-footer"}(
                div{class_ = "sync-footer-actions"}(
                    Snc::button({
                        .text = "Cancel"s,
                        .attributes = {
                            onClick = [this]() { cancel(); }
                        },
                        .styleVariant = Snc::StyleVariant::Danger,
                    })
                )
            )
        )
    );
    // clang-format on
}
