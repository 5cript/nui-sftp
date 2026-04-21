#pragma once

#include <nui-file-explorer/side.hpp>

#include <persistence/state_holder.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/terminal_options.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/archive_transfer_dialog.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/session_snapshot.hpp>
#include <shared_data/directory_entry.hpp>
#include <ids/ids.hpp>

class ProtoSession;

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/utility/stabilize.hpp>
#include <nui/utility/move_detector.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class Session
{
  public:
    /**
     * @brief All the wiring a Session needs at construction time.  Bundled
     *        into a struct to keep the call site readable — the field count
     *        long outgrew a linear argument list.
     */
    struct Params
    {
        /* Prop-drill pointers (non-owning) */
        Persistence::StateHolder* stateHolder = nullptr;
        FrontendEvents* events = nullptr;
        InputDialog* newItemAskDialog = nullptr;
        ConfirmDialog* confirmDialog = nullptr;
        FilePropertyDialog* filePropertyDialog = nullptr;
        ArchiveTransferDialog* archiveTransferDialog = nullptr;

        /* Configuration captured at add-time */
        Persistence::SessionOptions sessionOptions{};
        Persistence::UiOptions uiOptions{};
        std::string initialName{};
        std::optional<std::string> layoutName{};

        /* Callbacks into the owning SessionArea */
        /** @brief Called to remove this Session from its owner. */
        std::function<void(Session const*)> closeSelf{};
        /** @brief Resolves a desired tab title against sibling sessions' titles. */
        std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle{};
        /**
         * @brief Invoked when the user presses R in a locked terminal or
         *        clicks a Reconnect button in a connection-lost overlay.
         *        SessionArea responds by constructing a fresh Session bound
         *        to the same tabId and seeded with the given snapshot.
         */
        std::function<void(Session const*, SessionSnapshot)> requestReconnect{};
        /**
         * @brief Invoked when an in-progress reconnect (this Session was
         *        constructed with a resumeFromSnapshot) fails to open its
         *        SSH engine.  SessionArea schedules the next retry.
         */
        std::function<void(Session const*)> onReconnectFailed{};
        /**
         * @brief Invoked on successful reconnect application.  SessionArea
         *        tears down the dying predecessor Session and clears its
         *        per-tab retry state.
         */
        std::function<void(Session const*)> onReconnectSucceeded{};
        /**
         * @brief User clicked Cancel on the in-session reconnect overlay.
         *        SessionArea should drop the in-flight ProtoSession, clear
         *        timers, and call stopReconnectUi() on this Session so the
         *        overlay reverts to its idle [Reconnect] state.  Used only
         *        on the *old* (preserved) Session during a reconnect cycle.
         */
        std::function<void(Session const*)> onReconnectCancel{};
        /**
         * @brief User clicked Now on the in-session reconnect overlay.
         *        SessionArea should drop any scheduled backoff timer and
         *        fire the next attempt immediately.  Used only on the *old*
         *        (preserved) Session during a reconnect cycle.
         */
        std::function<void(Session const*)> onReconnectNow{};

        /* Initial visual state */
        bool visible = false;
        int tabId = -1;

        /**
         * @brief If present, this Session is a reconnect replacement for a
         *        previous one.  initializeLayout restores the saved Lumino
         *        layout, onOpenSession calls applySnapshot on success, and
         *        local-shell tabs in the restored layout are populated by
         *        adopting ejected processes instead of spawning fresh ones.
         */
        std::optional<SessionSnapshot> resumeFromSnapshot{};
    };

    explicit Session(Params params);
    /**
     * @brief Adoption constructor — consumes a ProtoSession that has already
     *        opened its transport.  @p params provides the Session-level
     *        wiring (stateHolder, events, dialogs, callbacks, tabId, …);
     *        sessionOptions, uiOptions and resumeFromSnapshot are moved out
     *        of the proto regardless of whatever @p params carried there.
     *        The ProtoSession is destroyed at the end of the constructor.
     */
    Session(Params params, std::unique_ptr<ProtoSession> proto);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Session);

    Nui::ElementRenderer operator()();

    /**
     * @brief initiates shutdown of the session manager and calls onShutdown when done.
     * Do not use "from the inside", is only used by session manager to close a session.
     *
     * @param onShutdown callback to call when shutdown is complete
     */
    void shutdown(std::function<void()> onShutdown);

    std::string name() const;
    std::string layoutId() const;
    std::weak_ptr<Nui::Observed<std::string>> tabTitle() const;
    void visible(bool value);
    bool visible() const;

    std::optional<std::string> getProcessIdIfExecutingEngine() const;
    auto makeChannelElement() -> Nui::ElementRenderer;
    auto makeLocalShellChannelElement(std::string const& shellName) -> Nui::ElementRenderer;
    /**
     * @brief Variant of makeLocalShellChannelElement that adopts an existing
     *        backend process instead of spawning a fresh one.  Used during the
     *        reconnect Lumino-layout restore to hand adopted channels to the
     *        new aux engine seamlessly.
     */
    auto makeAdoptedLocalShellChannelElement(LocalShellAdoption adoption) -> Nui::ElementRenderer;
    auto makeFileExplorerElement() -> Nui::ElementRenderer;
    auto makeOperationQueueElement() -> Nui::ElementRenderer;
    auto makeFileTrackingElement() -> Nui::ElementRenderer;

    /** @brief True if this session can host local-shell panels (SSH sessions only). */
    bool supportsLocalShell() const;

    /**
     * @brief Prepares to spawn a local-shell panel for the named saved shell.
     *
     * Stashes the shell name so the next `local-shell:<name>` fabrication can
     * look it up, then asks contentPanelManager to fulfil the pending add
     * request with the prefixed layout id. The actual process spawn happens
     * when localShellFactory (wired in initializeLayout) is invoked.
     */
    void openLocalShellChannel(std::string const& shellName);

    std::optional<nlohmann::json> getLayout() const;
    int tabId() const;

    /**
     * @brief True while the Session's SSH transport is considered lost and
     *        the user has not yet triggered (or committed to) a reconnect.
     */
    bool isInLostConnectionState() const;

    /**
     * @brief User-invoked reconnect trigger.  Captures a SessionSnapshot and
     *        hands it to the requestReconnect callback for SessionArea to
     *        build a fresh Session bound to the same tabId.  No-op outside
     *        the locked-connection state.
     */
    void reconnect();

    /**
     * @brief Extracts the pending reconnect snapshot out of the Session.
     *        Used by SessionArea when a reconnect attempt's Session fails to
     *        open — the snapshot is reused for the next retry.
     */
    std::optional<SessionSnapshot> takePendingSnapshot();

    /* ── In-session reconnect UI ────────────────────────────────────────── */
    /* The Session owns the overlay shown during a reconnect cycle so the
       dialog is no longer a viewport-blocking modal — each session reconnects
       independently.  SessionArea drives state changes into the Session; the
       Session's overlay renders from those observables and forwards user
       intent through Params::onReconnectCancel / onReconnectNow. */

    /** @brief Flip the overlay from idle [Reconnect] to the cycle UI. */
    void startReconnectUi();
    /** @brief Flip the overlay back to idle [Reconnect]. */
    void stopReconnectUi();
    /** @brief Update the displayed attempt counter (1-based). */
    void setReconnectUiAttempt(int attempt);
    /**
     * @brief Update the displayed countdown in seconds.  A value <= 0
     *        switches the UI to its "firing now" wording.
     */
    void setReconnectUiCountdown(int seconds);

    /**
     * @brief Used on windows to handle files dropped onto the session area.
     *
     * @param isLocalSide
     * @param entries
     * @param subdir
     */
    void
    onDrop(bool isLocalSide, std::vector<SharedData::DirectoryEntry> entries, std::optional<std::string> const& subdir);

    /**
     * @brief Ejects local-shell channels from the aux engine without any
     *        other snapshot work.  Used by SessionArea during the proto-
     *        session handoff: the non-destructive snapshot is captured at
     *        Reconnect click, and this runs at the moment of swap so the
     *        processes survive exactly the transition and no longer.
     */
    std::vector<LocalShellAdoption> ejectLocalShellsForHandoff();

  private:
    void onOpenSession(bool success, std::string const& info);
    void onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info);

    /**
     * @brief Captures the currently-visible state in a SessionSnapshot for
     *        transfer into a replacement Session.  The @p withEjection
     *        parameter controls whether local-shell channels are also
     *        evicted from the aux engine (a destructive step that leaves
     *        the backend processes alive but kills the frontend wrappers).
     *        SessionArea captures non-destructively at reconnect click and
     *        only ejects when the ProtoSession handoff is about to happen.
     */
    SessionSnapshot captureSnapshot(bool withEjection = true);

    /**
     * @brief Applies a snapshot produced by captureSnapshot to this Session.
     *        Called from onOpenSession on successful reconnect open.
     */
    void applySnapshot(SessionSnapshot const& snapshot);

    void onFileExplorerConnectionClose();
    void onTerminalConnectionLoss();
    /**
     * @brief Open the SFTP subsystem for this session.
     * @param username Remote username to use when formatting the default remote path.
     * @param forceOpen If true, bypass the `opts.openSftpByDefault` gate.  The
     *        reconnect path uses this so SFTP is re-opened whenever
     *        `snapshot.sftpWasOpen` was true, regardless of the user's
     *        "open by default" setting — otherwise `applySnapshot` runs with
     *        no `fileEngine` and every `enqueueResumable` is dropped.
     */
    void openSftp(std::string const& username, bool forceOpen = false);
    void openLocalFilesystem();
    void closeSelf();
    void initializeLayout();

    void setupFileGrid();

    void onChannelClosedByUser(Ids::ChannelId const& channelId);

    void createExecutingEngine();
    void createSshEngine();

    NuiFileExplorer::Side& localFileGridSide();
    NuiFileExplorer::Side* remoteFileGridSide();

    LocalSideModel& localSideModel();
    RemoteSideModel* remoteSideModel();

    void loadLayoutExtras(nlohmann::json const& layoutExtra);

    void onConnectionLoss();
    void onChannelLoss(Ids::ChannelId const& id);
    void onLockedModeUserInput(Ids::ChannelId channelId, std::string const& input);
    void saveTerminalContents(std::filesystem::path const& file, std::vector<std::string> const& contents);

    /** @brief On-demand "save the scrollback of @p channelId to a file the user picks". */
    void saveChannelToFile(Ids::ChannelId const& channelId);
    /** @brief On-demand "copy the raw xterm dump of @p channelId to the system clipboard". */
    void copyChannelToClipboard(Ids::ChannelId const& channelId);
    /**
     * @brief On-demand "copy @p channelId's scrollback as plain text" — ANSI
     *        escapes and non-whitespace control characters are stripped on the
     *        JS side before the clipboard write.  Useful for pasting into
     *        editors or tickets where formatting bytes would be noise.
     */
    void copyChannelToClipboardPlain(Ids::ChannelId const& channelId);

    /**
     * @brief The thin per-terminal row rendered directly above each xterm.
     *        Shows an identity glyph on the left (SSH vs local-shell flavour)
     *        and action buttons on the right (save, copy, copy-plain) wired to
     *        the channel whose id resolves out of @p channelIdCell.  Cell-based
     *        so that SSH and new-local-shell paths can pass a shared handle
     *        that fills in once onCreated fires; adopted local-shells seed the
     *        cell with the known process id.
     *        @p terminalBackgroundColor is the xterm theme's own background —
     *        the toolbar darkens it slightly for a distinct strip above the
     *        xterm so the bar is visually separated rather than blending in.
     */
    Nui::ElementRenderer renderTerminalToolbar(
        Nui::ElementRenderer identityIcon,
        std::shared_ptr<std::optional<Ids::ChannelId>> channelIdCell,
        std::string const& terminalBackgroundColor
    );

    /**
     * @brief Body renderer for the Session's lost-connection dialog.
     *        Swaps between the idle [Reconnect] affordance and the
     *        in-progress reconnect-cycle UI (attempt counter, countdown,
     *        [Now], [Cancel]) based on the Session's reconnectCycleActive
     *        observable.  The enclosing dialog provides its own header;
     *        this produces only the body content.
     */
    Nui::ElementRenderer makeConnectionLostDialogBody();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};