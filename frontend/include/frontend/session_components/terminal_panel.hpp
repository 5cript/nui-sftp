#pragma once

#include <persistence/state/session_options.hpp>
#include <persistence/state/terminal_options.hpp>
#include <persistence/state/termios.hpp>
#include <frontend/session_snapshot.hpp>
#include <ids/ids.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/dom/element.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Persistence
{
    class StateHolder;
}
class ConfirmDialog;
class FrontendSessionManager;

/**
 * @brief Owns the per-terminal-tab factories (SSH channel, local-shell, and
 *        adopted local-shell) along with the toolbar action handlers, the
 *        locked-mode keyboard handler, and the per-channel metadata needed to
 *        snapshot local shells across a reconnect.  Extracted from Session as
 *        part of the session.cpp refactor (Step 3).
 */
class TerminalPanel
{
  public:
    /**
     * @brief Per-local-shell metadata tracked from spawn through handoff.
     *        Populated by makeLocalShellChannelElement on open and by
     *        makeAdoptedLocalShellChannelElement on reconnect; consumed by
     *        Session::ejectLocalShellsForHandoff when building the new
     *        snapshot's adoption list.
     */
    struct LocalShellMeta
    {
        std::string shellConfigName;
        std::string cmdline;
        Persistence::TerminalOptions terminalOptions;
        Persistence::Termios termios;
        Persistence::ExecutingSessionOptions execOpts;
    };

    /**
     * @brief Wiring for TerminalPanel.  Bundled so the call site does not
     *        degrade into a positional argument marathon as the surface grows.
     */
    struct Params
    {
        Persistence::StateHolder* stateHolder = nullptr;
        ConfirmDialog* confirmDialog = nullptr;

        /**
         * @brief Non-owning reference to the Session's FSM observable.  The
         *        factories observe it so their subtrees rebuild whenever the
         *        underlying FrontendSessionManager is replaced (connect,
         *        reconnect, shutdown).
         */
        Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager = nullptr;

        /**
         * @brief Non-owning reference to the Session's engine configuration
         *        (read for the initial toolbar icon and for engine-type
         *        discrimination in makeChannelElement).
         */
        Persistence::SessionOptions const* engineOptions = nullptr;

        /** @brief Lumino container id; used by the *:renameTerminalById JS calls. */
        std::string sessionLayoutId;

        /**
         * @brief Predicate: is the owning Session in the lost-connection
         *        state?  Used by onChannelLoss to decide whether to suppress
         *        the Lumino tab close (so the user can still save contents).
         */
        std::function<bool()> isInLostConnectionState;

        /** @brief Fires when the user presses R in a locked-mode terminal. */
        std::function<void()> onReconnectRequested;

        /** @brief Fires when the user presses Enter in a locked-mode terminal. */
        std::function<void()> onCloseSelfRequested;

        /**
         * @brief Fires after each channel-creation callback (success or
         *        failure).  Session uses it to drain pendingScrollbackReplay
         *        into newly-opened primary channels.  The creation-failure
         *        branch is handled internally before this fires.
         */
        std::function<void(std::optional<Ids::ChannelId>, std::string const&)> onChannelOpened;
    };

    explicit TerminalPanel(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(TerminalPanel);

    /** @brief Factory for an SSH / executing-engine terminal tab. */
    Nui::ElementRenderer makeChannelElement();

    /** @brief Factory for a freshly-spawned local-shell terminal tab. */
    Nui::ElementRenderer makeLocalShellChannelElement(std::string const& shellName);

    /** @brief Factory for a local-shell terminal tab that adopts a backend process. */
    Nui::ElementRenderer makeAdoptedLocalShellChannelElement(LocalShellAdoption adoption);

    /** @brief Save the channel's scrollback to a user-picked file. */
    void saveChannelToFile(Ids::ChannelId const& channelId);
    /** @brief Copy the channel's scrollback (with ANSI codes) to the clipboard. */
    void copyChannelToClipboard(Ids::ChannelId const& channelId);
    /** @brief Copy the channel's scrollback as plain text (ANSI stripped on JS side). */
    void copyChannelToClipboardPlain(Ids::ChannelId const& channelId);

    /**
     * @brief Handler for terminal keyboard input while the session is in
     *        locked mode: R = reconnect, S = save-all, Enter = close.
     *        The R / Enter branches forward through Params callbacks.
     */
    void onLockedModeUserInput(Ids::ChannelId channelId, std::string const& input);

    /**
     * @brief Dumps every primary channel's scrollback into the locked-mode
     *        buffer so a subsequent S-key press can save it.  Called by
     *        Session::onConnectionLoss.
     */
    void captureChannelContentsForLockedMode();

    /**
     * @brief Handles a failed channel-creation callback: pops the last
     *        tracked channel element, closes its Lumino tab, and opens the
     *        failure confirm dialog.
     */
    void onChannelCreationFailed(std::string const& info);

    /**
     * @brief Handles channel loss (process death or SSH transport drop).
     *        Erases the channel's LocalShellMeta and, unless the owning
     *        Session is in lost-connection state (for primary channels),
     *        closes the Lumino tab after a short debounce delay.
     */
    void onChannelLoss(Ids::ChannelId const& id);

    /**
     * @brief Handles a user-initiated tab close: drops the channel's DOM
     *        element from the tracking vector, closes the FSM channel, and
     *        erases the channel's LocalShellMeta.
     */
    void onChannelClosedByUser(Ids::ChannelId const& channelId);

    /**
     * @brief The vector the layout initializer pushes each terminal element
     *        into (keeps them alive while the Lumino tab exists).  Exposed
     *        by reference so SessionLayoutInitializer can append, and so
     *        onChannelCreationFailed / onChannelClosedByUser can prune.
     */
    std::vector<std::shared_ptr<Nui::Dom::Element>>& channelElements();

    /** @brief Read access to a channel's LocalShellMeta, or nullptr if absent. */
    LocalShellMeta const* findLocalShellMeta(Ids::ChannelId const& channelId) const;

    /** @brief Drop all LocalShellMeta entries (called after ejection). */
    void clearLocalShellMeta();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
