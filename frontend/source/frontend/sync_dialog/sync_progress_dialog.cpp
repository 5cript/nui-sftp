#include <frontend/sync_dialog/sync_progress_dialog.hpp>
#include <frontend/components/icon_panel.hpp>
#include <frontend/components/progress_bar.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/val.hpp>
#include <nui/utility/move_detector.hpp>

#include <ui5-sap-icons/icons/synchronize.hpp>

#include <shared_data/sync_phase.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct SyncProgressDialog::Implementation
{
    Nui::Observed<bool> open_{false};
    Nui::Observed<SharedData::SyncPhase> phase_{SharedData::SyncPhase::Idle};

    // Listing counters
    Nui::Observed<std::uint64_t> localListed_{0ull};
    Nui::Observed<std::uint64_t> remoteListed_{0ull};

    // Comparing counter
    Nui::Observed<std::uint64_t> compared_{0ull};

    // Hashing progress bar (total becomes known when comparing finishes)
    Components::ProgressBar hashProgressBar_{Components::ProgressBar::Settings{
        .height = "10px",
        .min = 0,
        .max = 0,
        .showMinMax = true,
    }};

    std::filesystem::path localPath_{};
    std::filesystem::path remotePath_{};
    std::function<void()> onDone_{};

    std::shared_ptr<bool> cancelToken_{std::make_shared<bool>(false)};
};

SyncProgressDialog::SyncProgressDialog()
    : impl_{std::make_unique<Implementation>()}
{}

SyncProgressDialog::~SyncProgressDialog() = default;
SyncProgressDialog::SyncProgressDialog(SyncProgressDialog&&) = default;
SyncProgressDialog& SyncProgressDialog::operator=(SyncProgressDialog&&) = default;

void SyncProgressDialog::open(
    std::filesystem::path localPath,
    std::filesystem::path remotePath,
    std::function<void()> onDone
)
{
    // Cancel any in-progress simulation
    *impl_->cancelToken_ = true;
    auto token = std::make_shared<bool>(false);
    impl_->cancelToken_ = token;

    impl_->localPath_ = std::move(localPath);
    impl_->remotePath_ = std::move(remotePath);
    impl_->onDone_ = std::move(onDone);

    // Reset state
    impl_->phase_ = SharedData::SyncPhase::Listing;
    impl_->localListed_ = 0;
    impl_->remoteListed_ = 0;
    impl_->compared_ = 0;
    impl_->hashProgressBar_.max(0);
    impl_->open_ = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    // --- Demo simulation ---

    // Tick listing every 120ms, 8 ticks each side
    for (int idx = 1; idx <= 8; ++idx)
    {
        Nui::val::global("setTimeout")
            .call<void>(
                "call",
                Nui::val::global("window"),
                Nui::bind(
                    [this, token, idx](Nui::val)
                    {
                        if (*token)
                            return;
                        impl_->localListed_ = static_cast<std::uint64_t>(idx * 3);
                        impl_->remoteListed_ = static_cast<std::uint64_t>(idx * 3 + 1);
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    },
                    std::placeholders::_1
                ),
                idx * 120
            );
    }

    // Switch to Comparing at ~1100ms
    Nui::val::global("setTimeout")
        .call<void>(
            "call",
            Nui::val::global("window"),
            Nui::bind(
                [this, token](Nui::val)
                {
                    if (*token)
                        return;
                    impl_->phase_ = SharedData::SyncPhase::Comparing;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
                std::placeholders::_1
            ),
            1100
        );

    // Tick comparing every 150ms, 6 ticks
    for (int idx = 1; idx <= 6; ++idx)
    {
        Nui::val::global("setTimeout")
            .call<void>(
                "call",
                Nui::val::global("window"),
                Nui::bind(
                    [this, token, idx](Nui::val)
                    {
                        if (*token)
                            return;
                        impl_->compared_ = static_cast<std::uint64_t>(idx * 4);
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    },
                    std::placeholders::_1
                ),
                1100 + idx * 150
            );
    }

    // Switch to Hashing at ~2100ms, set known total = 5
    Nui::val::global("setTimeout")
        .call<void>(
            "call",
            Nui::val::global("window"),
            Nui::bind(
                [this, token](Nui::val)
                {
                    if (*token)
                        return;
                    impl_->hashProgressBar_.max(5);
                    impl_->phase_ = SharedData::SyncPhase::Hashing;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                },
                std::placeholders::_1
            ),
            2100
        );

    // Hash files one by one, 250ms apart
    for (int idx = 1; idx <= 5; ++idx)
    {
        Nui::val::global("setTimeout")
            .call<void>(
                "call",
                Nui::val::global("window"),
                Nui::bind(
                    [this, token, idx](Nui::val)
                    {
                        if (*token)
                            return;
                        impl_->hashProgressBar_.setProgress(idx);
                        Nui::globalEventContext.executeActiveEventsImmediately();
                    },
                    std::placeholders::_1
                ),
                2100 + idx * 250
            );
    }

    // Done at ~3500ms
    Nui::val::global("setTimeout")
        .call<void>(
            "call",
            Nui::val::global("window"),
            Nui::bind(
                [this, token](Nui::val)
                {
                    if (*token)
                        return;
                    impl_->phase_ = SharedData::SyncPhase::Done;
                    impl_->open_ = false;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                    if (impl_->onDone_)
                        impl_->onDone_();
                },
                std::placeholders::_1
            ),
            3500
        );
}

void SyncProgressDialog::cancel()
{
    *impl_->cancelToken_ = true;
    impl_->cancelToken_ = std::make_shared<bool>(false);
    impl_->open_ = false;
    impl_->phase_ = SharedData::SyncPhase::Idle;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

Nui::ElementRenderer SyncProgressDialog::operator()()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using namespace Nui::Attributes::Literals;
    using Nui::Elements::div;
    using Nui::Elements::span;

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
                button{
                    class_ = "icon-button sync-close-x",
                    onClick = [this](Nui::val) { cancel(); }
                }("x")
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
                    button{
                        class_ = "icon-button",
                        onClick = [this](Nui::val) { cancel(); }
                    }("Cancel")
                )
            )
        )
    );
    // clang-format on
}
