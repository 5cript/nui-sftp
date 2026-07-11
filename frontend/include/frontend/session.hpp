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
#include <frontend/command_store/command_store_client.hpp>
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
    /** @brief Construction-time wiring. */
    struct Params
    {
        /* Prop-drill pointers (non-owning) */
        Persistence::StateHolder* stateHolder = nullptr;
        FrontendEvents* events = nullptr;
        InputDialog* newItemAskDialog = nullptr;
        ConfirmDialog* confirmDialog = nullptr;
        FilePropertyDialog* filePropertyDialog = nullptr;
        ArchiveTransferDialog* archiveTransferDialog = nullptr;
        /** @brief The process global command store; commands executed in this session are recorded there. */
        CommandStoreClient* commandStoreClient = nullptr;

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
        /** @brief Locked-terminal R / overlay Reconnect: SessionArea builds a fresh Session from the snapshot. */
        std::function<void(Session const*, SessionSnapshot)> requestReconnect{};
        /** @brief In-progress reconnect's SSH engine failed to open; SessionArea schedules the next retry. */
        std::function<void(Session const*)> onReconnectFailed{};
        /** @brief Reconnect applied successfully; SessionArea tears down the predecessor. */
        std::function<void(Session const*)> onReconnectSucceeded{};
        /** @brief Overlay Cancel (on the preserved Session during a cycle). */
        std::function<void(Session const*)> onReconnectCancel{};
        /** @brief Overlay Now (skip remaining backoff; on the preserved Session). */
        std::function<void(Session const*)> onReconnectNow{};

        /* Initial visual state */
        bool visible = false;
        int tabId = -1;

        /** @brief Seeds a reconnect replacement with the captured snapshot. */
        std::optional<SessionSnapshot> resumeFromSnapshot{};
    };

    explicit Session(Params params);
    /**
     * @brief Adoption constructor: consumes a ProtoSession that already
     *        opened its transport.  Engine + UI options and (if unset)
     *        resume snapshot are moved out of the proto.
     */
    Session(Params params, std::unique_ptr<ProtoSession> proto);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Session);

    Nui::ElementRenderer operator()();

    /**
     * @brief Initiates shutdown and calls @p onShutdown when complete.
     *        Called by SessionManager; not for in-session use.
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
    /** @brief Adopts an existing backend process during reconnect layout restore. */
    auto makeAdoptedLocalShellChannelElement(LocalShellAdoption adoption) -> Nui::ElementRenderer;
    auto makeFileExplorerElement() -> Nui::ElementRenderer;

    /** @brief True if this session can host local-shell panels (SSH sessions only). */
    bool supportsLocalShell() const;

    /**
     * @brief Asks contentPanelManager to fabricate a `local-shell:<name>`
     *        panel.  The actual process spawn happens in localShellFactory.
     */
    void openLocalShellChannel(std::string const& shellName);

    std::optional<nlohmann::json> getLayout() const;
    int tabId() const;

    /** @brief True while the SSH transport is lost and no reconnect has succeeded yet. */
    bool isInLostConnectionState() const;

    /** @brief Captures a snapshot and hands off to SessionArea via requestReconnect. */
    void reconnect();

    /** @brief Extracts the pending snapshot (used by SessionArea on failed retry). */
    std::optional<SessionSnapshot> takePendingSnapshot();

    /* In-session reconnect UI: SessionArea drives these setters; the
       overlay renders from them and forwards user intent via Params. */

    /** @brief Flip the overlay from idle [Reconnect] to the cycle UI. */
    void startReconnectUi();
    /** @brief Flip the overlay back to idle [Reconnect]. */
    void stopReconnectUi();
    /** @brief Update the displayed attempt counter (1-based). */
    void setReconnectUiAttempt(int attempt);
    /** @brief Update displayed countdown; <=0 switches to "firing now" wording. */
    void setReconnectUiCountdown(int seconds);

    /** @brief Handles files dropped onto the session area (Windows). */
    void
    onDrop(bool isLocalSide, std::vector<SharedData::DirectoryEntry> entries, std::optional<std::string> const& subdir);

    /**
     * @brief Ejects local-shell channels from the aux engine.  Used by
     *        SessionArea at the ProtoSession handoff moment so processes
     *        survive the transition.
     */
    std::vector<LocalShellAdoption> ejectLocalShellsForHandoff();

  private:
    void onOpenSession(bool success, std::string const& info);
    void onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info);

    /**
     * @brief Captures current state for transfer into a replacement Session.
     *        @p withEjection also evicts local-shell channels (destructive);
     *        SessionArea captures non-destructively first, ejects at swap.
     */
    SessionSnapshot captureSnapshot(bool withEjection = true);

    /** @brief Applies a captured snapshot; called from onOpenSession on reconnect. */
    void applySnapshot(SessionSnapshot const& snapshot);

    void onTerminalConnectionLoss();
    /**
     * @brief Opens the SFTP subsystem.  @p forceOpen bypasses the
     *        `openSftpByDefault` gate: required on reconnect so applySnapshot
     *        can re-enqueue operations into a live FileEngine.
     */
    void openSftp(std::string const& username, bool forceOpen = false);
    void openLocalFilesystem();
    void closeSelf();

    void setupFileGrid();

    void onChannelClosedByUser(Ids::ChannelId const& channelId);

    void createExecutingEngine();
    void createSshEngine();

    /**
     * @brief Points the session manager at the command store: capture mode and the sink that records
     *        what was executed. Call once the manager exists, before any channel is created.
     */
    void wireCommandCapture();

    NuiFileExplorer::Side& localFileGridSide();
    NuiFileExplorer::Side* remoteFileGridSide();

    LocalSideModel& localSideModel();
    RemoteSideModel* remoteSideModel();

    void onConnectionLoss();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};