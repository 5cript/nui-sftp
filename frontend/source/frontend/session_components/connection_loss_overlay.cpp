#include <frontend/session_components/connection_loss_overlay.hpp>

#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/spinner.hpp>

#include <fmt/format.h>

#include <nui/event_system/event_context.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/event_system/observed_value_combinator.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements/nil.hpp>

#include <utility>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

struct ConnectionLossOverlay::Implementation
{
    std::function<void()> onReconnectClicked;
    std::function<void()> onReconnectNowClicked;
    std::function<void()> onReconnectCancelClicked;

    Nui::Observed<bool> reconnectCycleActive{false};
    Nui::Observed<int> reconnectAttempt{1};
    Nui::Observed<int> reconnectCountdown{0};

    /**
     * @brief Non-modal, draggable dialog that hosts the reconnect UI.
     *        Opened on connection loss, closed on Session disposal.  The
     *        draggability lets the user move it out of the way to keep
     *        working with local-shell panels while the SSH transport tries
     *        to reconnect.
     */
    std::unique_ptr<ScriptNuiComponents::Dialog> dialog;

    explicit Implementation(Params&& params)
        : onReconnectClicked{std::move(params.onReconnectClicked)}
        , onReconnectNowClicked{std::move(params.onReconnectNowClicked)}
        , onReconnectCancelClicked{std::move(params.onReconnectCancelClicked)}
    {}
};

Nui::ElementRenderer ConnectionLossOverlay::makeBody(Implementation& impl)
{
        using Nui::Elements::div;
        using Nui::Elements::span;
        namespace Snc = ScriptNuiComponents;

        // clang-format off
        return div{class_ = "session-reconnect-panel-body"}(
            observe(impl.reconnectCycleActive),
            [&impl]() -> Nui::ElementRenderer {
                using Nui::Elements::div;
                using Nui::Elements::span;
                namespace Snc = ScriptNuiComponents;

                if (!impl.reconnectCycleActive.value())
                {
                    return Snc::button({
                        .text = language->getObserved("sessionFrontend", "reconnectButton"),
                        .attributes = {onClick = [&impl]() {
                            if (impl.onReconnectClicked)
                                impl.onReconnectClicked();
                        }},
                        .styleVariant = Snc::StyleVariant::Primary,
                    });
                }

                // Cycle UI: spinner + attempt + countdown + [Now] + [Cancel].
                // Wired to Params::onReconnectNow / onReconnectCancel the
                // retry timers and the candidate ProtoSession live in
                // SessionArea, so this widget's only job is to hand user
                // intent upward.
                return div{class_ = "session-reconnect-cycle"}(
                    div{class_ = "session-reconnect-cycle-row"}(
                        Snc::spinner({.size = "22px", .thickness = "3px", .color = std::nullopt}),
                        span{}(
                            observe(impl.reconnectAttempt),
                            [&impl]() {
                                return fmt::format(
                                    fmt::runtime(language->get("reconnectDialog", "attempt")),
                                    impl.reconnectAttempt.value()
                                );
                            }
                        )
                    ),
                    div{class_ = "session-reconnect-cycle-row"}(
                        observe(impl.reconnectCountdown),
                        [&impl]() -> Nui::ElementRenderer {
                            if (impl.reconnectCountdown.value() <= 0)
                                return span{}(language->getObserved("reconnectDialog", "restoringState"));
                            return span{}(
                                fmt::format(
                                    fmt::runtime(language->get("reconnectDialog", "retryIn")),
                                    impl.reconnectCountdown.value()
                                )
                            );
                        }
                    ),
                    div{class_ = "session-reconnect-cycle-buttons"}(
                        observe(impl.reconnectCountdown),
                        [&impl]() -> Nui::ElementRenderer {
                            using Nui::Elements::div;
                            namespace Snc = ScriptNuiComponents;
                            // Hide [Now] while the retry is already firing
                            // (countdown == 0 → "Restoring..." state) the
                            // click would be a no-op there.
                            auto nowButton = (impl.reconnectCountdown.value() > 0)
                                ? Snc::button({
                                      .text = language->getObserved("reconnectDialog", "now"),
                                      .attributes = {onClick = [&impl]() {
                                          if (impl.onReconnectNowClicked)
                                              impl.onReconnectNowClicked();
                                      }},
                                      .styleVariant = Snc::StyleVariant::Primary,
                                  })
                                : Nui::ElementRenderer{Nui::nil()};
                            return div{class_ = "session-reconnect-cycle-buttons-inner"}(
                                std::move(nowButton),
                                Snc::button({
                                    .text = language->getObserved("reconnectDialog", "cancel"),
                                    .attributes = {onClick = [&impl]() {
                                        if (impl.onReconnectCancelClicked)
                                            impl.onReconnectCancelClicked();
                                    }},
                                    .styleVariant = Snc::StyleVariant::Regular,
                                })
                            );
                        }
                    )
                );
            }
        );
    // clang-format on
}

ConnectionLossOverlay::ConnectionLossOverlay(Params params)
{
    // Capture dialog id before moving the Params into the Implementation; the
    // moved-from Params state is valid-but-unspecified.
    auto dialogId = params.sessionLayoutId + "-reconnect-dialog";
    impl_ = std::make_unique<Implementation>(std::move(params));
    impl_->dialog = std::make_unique<ScriptNuiComponents::Dialog>(
        std::move(dialogId),
        makeBody(*impl_)
    );
}
ConnectionLossOverlay::~ConnectionLossOverlay() = default;
ConnectionLossOverlay::ConnectionLossOverlay(ConnectionLossOverlay&&) = default;
ConnectionLossOverlay& ConnectionLossOverlay::operator=(ConnectionLossOverlay&&) = default;

Nui::ElementRenderer ConnectionLossOverlay::operator()()
{
    return (*impl_->dialog)();
}

void ConnectionLossOverlay::show()
{
    if (!impl_->dialog)
        return;
    // Show the per-session reconnect dialog.  Non-modal so local-shell
    // panels (and other tabs) keep working; draggable so the user can
    // move it out of the way.  The dialog's lifetime is bound to the
    // Session we never explicitly close it since the Session replace
    // tears its DOM down when the reconnect succeeds.
    impl_->dialog->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("sessionFrontend", "connectionLost"),
        .buttons = ScriptNuiComponents::Dialog::Button::Unknown,
        .modal = false,
        .mayCloseWithoutButton = true,
        .draggable = true,
    });
}

bool ConnectionLossOverlay::isReconnectCycleActive() const
{
    return impl_->reconnectCycleActive.value();
}

void ConnectionLossOverlay::startReconnectUi()
{
    impl_->reconnectCycleActive = true;
    impl_->reconnectAttempt = 1;
    impl_->reconnectCountdown = 0;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void ConnectionLossOverlay::stopReconnectUi()
{
    impl_->reconnectCycleActive = false;
    impl_->reconnectAttempt = 1;
    impl_->reconnectCountdown = 0;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void ConnectionLossOverlay::setReconnectUiAttempt(int attempt)
{
    impl_->reconnectAttempt = attempt;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void ConnectionLossOverlay::setReconnectUiCountdown(int seconds)
{
    impl_->reconnectCountdown = seconds;
    Nui::globalEventContext.executeActiveEventsImmediately();
}
