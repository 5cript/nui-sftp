#include <frontend/sync_dialog/sync_progress_dialog.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/components/progress_bar.hpp>
#include <frontend/svgs/decline.hpp>

#include <utility/language.hpp>

#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
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
#include <script-nui-components/spinner.hpp>
#include <script-nui-components/style_variant.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

    BackendSyncProvider* provider_{nullptr};
    SharedData::Sync::DiffOptions initialOptions_{};
    std::function<void(SharedData::Sync::DiffSummary)> onDone_{};

    // Cancel token; replaced on each open() to invalidate stale callback captures.
    // The provider also has its own cancel flag that kills the backend walk — this
    // one only guards the frontend lambdas.
    std::shared_ptr<bool> cancelToken_{std::make_shared<bool>(false)};
};

SyncProgressDialog::SyncProgressDialog(OperationQueue* operationQueue)
    : operationQueue_{operationQueue}
    , impl_{std::make_unique<Implementation>()}
{}

SyncProgressDialog::~SyncProgressDialog() = default;
SyncProgressDialog::SyncProgressDialog(SyncProgressDialog&&) = default;
SyncProgressDialog& SyncProgressDialog::operator=(SyncProgressDialog&&) = default;

void SyncProgressDialog::open(
    BackendSyncProvider* provider,
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    bool respectIgnoreFiles,
    bool recursive,
    bool ignoreHidden,
    SharedData::Sync::DiffOptions initialOptions,
    std::function<void(SharedData::Sync::DiffSummary)> onDone
)
{
    // Invalidate any stale callbacks from a previous open() by flipping the old
    // token and swapping in a fresh one.  (The new backend provider has its own
    // cancel flag that will also bail the current walk if one is running.)
    *impl_->cancelToken_ = true;
    auto token = std::make_shared<bool>(false);
    impl_->cancelToken_ = token;

    impl_->provider_ = provider;
    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);
    impl_->initialOptions_ = initialOptions;
    impl_->onDone_ = std::move(onDone);

    // Reset state
    impl_->phase_ = SharedData::SyncPhase::Listing;
    impl_->localListed_ = 0ull;
    impl_->remoteListed_ = 0ull;
    impl_->compared_ = 0ull;
    impl_->hashProgressBar_.max(0);
    impl_->open_ = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    provider->open(
        impl_->localPath_,
        impl_->remotePath_,
        respectIgnoreFiles,
        recursive,
        ignoreHidden,
        // onLocalListing
        [this, token](SharedData::ScanProgress const& progress)
        {
            if (*token)
                return;
            impl_->localListed_ = progress.totalScanned;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        // onRemoteListing
        [this, token](SharedData::ScanProgress const& progress)
        {
            if (*token)
                return;
            impl_->remoteListed_ = progress.totalScanned;
            Nui::globalEventContext.executeActiveEventsImmediately();
        },
        // onBothListed
        [this, token]()
        {
            if (*token)
                return;
            impl_->phase_ = SharedData::SyncPhase::Comparing;
            Nui::globalEventContext.executeActiveEventsImmediately();
            impl_->provider_->recompute(
                impl_->initialOptions_,
                [this, token](SharedData::Sync::DiffSummary summary)
                {
                    if (*token)
                        return;
                    impl_->phase_ = SharedData::SyncPhase::Done;
                    impl_->open_ = false;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    if (impl_->onDone_)
                        impl_->onDone_(std::move(summary));
                }
            );
        },
        // onDiffProgress
        [this, token](std::uint64_t compared)
        {
            if (*token)
                return;
            impl_->compared_ = compared;
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void SyncProgressDialog::cancel()
{
    *impl_->cancelToken_ = true;
    impl_->cancelToken_ = std::make_shared<bool>(false);
    impl_->open_ = false;
    impl_->phase_ = SharedData::SyncPhase::Idle;
    if (impl_->provider_)
        impl_->provider_->cancelDiff();
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
                            fmt::runtime(language->get("syncProgressDialog", "titleFormat")),
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
                                Snc::spinner({.size = "18px", .thickness = "3px", .color = std::nullopt}),
                                span{class_ = "sync-progress-step-label"}(language->getObserved("syncProgressDialog", "listingFiles"))
                            ),
                            div{class_ = "sync-progress-counters"}(
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}(language->getObserved("syncProgressDialog", "localLabel")),
                                    span{class_ = "sync-progress-counter-value"}(impl_->localListed_)
                                ),
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}(language->getObserved("syncProgressDialog", "remoteLabel")),
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
                                Snc::spinner({.size = "18px", .thickness = "3px", .color = std::nullopt}),
                                span{class_ = "sync-progress-step-label"}(language->getObserved("syncProgressDialog", "comparing"))
                            ),
                            div{class_ = "sync-progress-counters"}(
                                div{class_ = "sync-progress-counter"}(
                                    span{class_ = "sync-progress-counter-label"}(language->getObserved("syncProgressDialog", "differencesFound")),
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
                                span{class_ = "sync-progress-step-label"}(language->getObserved("syncProgressDialog", "hashing"))
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
                        .text = language->getObserved("syncProgressDialog", "cancel"),
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
