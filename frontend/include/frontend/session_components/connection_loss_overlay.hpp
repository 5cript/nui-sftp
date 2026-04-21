#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <string>

/**
 * @brief Per-session overlay shown when the SSH transport is lost.  Swaps
 *        between an idle [Reconnect] button and an in-progress cycle UI
 *        (attempt counter + countdown + [Now] / [Cancel]).  Non-modal and
 *        draggable so the user can keep working with local-shell panels
 *        and other tabs while the retry cycle runs.  Extracted from
 *        Session as part of the session.cpp refactor (Step 5).
 */
class ConnectionLossOverlay
{
  public:
    /**
     * @brief Wiring for ConnectionLossOverlay.  The three callbacks forward
     *        user intent to Session; Session still owns the business logic
     *        (snapshot capture, SessionArea plumbing, timers).
     */
    struct Params
    {
        /** @brief Lumino dialog id prefix the overlay appends its own suffix. */
        std::string sessionLayoutId;

        /** @brief User clicked the idle [Reconnect] button. */
        std::function<void()> onReconnectClicked;

        /** @brief User clicked [Now] during the cycle (skip remaining countdown). */
        std::function<void()> onReconnectNowClicked;

        /** @brief User clicked [Cancel] during the cycle (abort the retry). */
        std::function<void()> onReconnectCancelClicked;
    };

    explicit ConnectionLossOverlay(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ConnectionLossOverlay);

    /** @brief Produces the dialog + reactive body renderer. */
    Nui::ElementRenderer operator()();

    /** @brief Opens the dialog called by Session::onConnectionLoss. */
    void show();

    /**
     * @brief True while the retry cycle UI (spinner + attempt + countdown) is
     *        displayed.  Session's reconnect() guards against re-entry using
     *        this flag.
     */
    bool isReconnectCycleActive() const;

    /** @brief Flip the overlay from idle [Reconnect] to the cycle UI. */
    void startReconnectUi();
    /** @brief Flip the overlay back to idle [Reconnect]. */
    void stopReconnectUi();
    /** @brief Update the displayed attempt counter (1-based). */
    void setReconnectUiAttempt(int attempt);
    /**
     * @brief Update the displayed countdown in seconds.  A value <= 0
     *        switches the cycle wording to "restoring state" (no [Now]).
     */
    void setReconnectUiCountdown(int seconds);

  private:
    struct Implementation;
    static Nui::ElementRenderer makeBody(Implementation& impl);
    std::unique_ptr<Implementation> impl_;
};
