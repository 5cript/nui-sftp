#include <frontend/session.hpp>
#include <frontend/classes.hpp>
#include <frontend/proto_session.hpp>
#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/sync_progress_dialog.hpp>
#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/remote_engine_factory.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session_components/session_options.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/command_history_panel.hpp>
#include <frontend/session_components/command_snippets_panel.hpp>
#include <frontend/session_components/terminal_panel.hpp>
#include <frontend/session_components/file_explorer_panel.hpp>
#include <frontend/session_components/connection_loss_overlay.hpp>
#include <frontend/session_components/session_layout_initializer.hpp>
#include <frontend/session_components/session_snapshot_manager.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/notifications.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/sftp_options.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>
#include <utility/lifetime_token.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/frontend/rpc_client.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/elements/nil.hpp>
#include <nui/frontend/attributes.hpp>

#include <utility>
#include <vector>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

namespace
{
    /**
     * @brief Formats the address of a remote session as user@host:port, with brackets around IPv6 hosts.
     */
    std::string remoteAddress(std::string const& user, std::string const& host, int port)
    {
        if (host.find(':') != std::string::npos)
            return fmt::format("{}@[{}]:{}", user, host, port);
        return fmt::format("{}@{}:{}", user, host, port);
    }

    /**
     * @brief The user an FTP session logs in as, "anonymous" for a user without a name.
     */
    std::string ftpUserName(Persistence::FtpSessionOptions const& options)
    {
        const auto name = options.loginUser().name;
        if (!name || name->empty())
            return "anonymous";
        return *name;
    }

    /**
     * @brief The transfer options of a remote session, nothing for a local one.
     */
    Persistence::SftpOptions const* transferOptionsOf(Persistence::SessionOptions const& options)
    {
        if (auto const* ssh = std::get_if<Persistence::SshSessionOptions>(&options.engine))
            return &ssh->sftpOptions.value();
        if (auto const* ftp = std::get_if<Persistence::FtpSessionOptions>(&options.engine))
            return &ftp->sftpOptions.value();
        return nullptr;
    }

    /**
     * @brief Whether the remote file system opens with the session. Always for FTP, which has nothing else.
     */
    bool opensFileSystemByDefault(Persistence::SessionOptions const& options)
    {
        if (auto const* ssh = std::get_if<Persistence::SshSessionOptions>(&options.engine))
            return ssh->openSftpByDefault;
        return std::holds_alternative<Persistence::FtpSessionOptions>(options.engine);
    }
}

struct Session::Implementation
{
    // Prop-drill
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    ConfirmDialog* confirmDialog;
    CommandStoreClient* commandStoreClient;

    // Tab identity
    std::string initialName;
    /** @brief The name of the session in the settings, nothing for a direct connect. */
    std::optional<std::string> persistenceName;
    std::shared_ptr<Nui::Observed<std::string>> tabTitle;
    std::string sessionLayoutId;
    std::function<void(Session const*)> closeSelf;
    Nui::Observed<bool> isVisible;
    int tabId;
    Persistence::UiOptions uiOptions;

    // Engine config
    Persistence::SessionOptions engineOptions;
    std::optional<std::string> layoutName;

    /**
     * @brief True while the SSH transport is considered lost.  Declared ahead
     *        of the widgets that observe it so they can take a stable pointer
     *        during their own construction.
     */
    Nui::Observed<bool> isInLostConnectionState{false};

    // Widgets
    FileExplorerPanel fileExplorerPanel;
    OperationQueue operationQueue;
    Nui::Observed<std::unique_ptr<FrontendSessionManager>> frontendSessionManager;
    std::unique_ptr<TerminalPanel> terminalPanel;
    SessionOptions sessionOptions;
    FileTrackingPanel fileTrackingPanel;
    CommandHistoryPanel commandHistoryPanel;
    CommandSnippetsPanel commandSnippetsPanel;
    std::unique_ptr<SessionLayoutInitializer> layoutInitializer;
    SyncDialog syncDialog;
    SyncProgressDialog syncProgressDialog;
    // Last local+remote paths given to startSyncFlow; used by the dialog's
    // Recompare path to re-issue the flow without asking the user again.
    std::filesystem::path lastSyncLocalPath_{};
    std::filesystem::path lastSyncRemotePath_{};
    std::unique_ptr<ConnectionLossOverlay> connectionLossOverlay;
    std::unique_ptr<SessionSnapshotManager> snapshotManager;

    // Shutdown
    Nui::Observed<bool> inertEverything{false};
    std::function<void()> onShutdownComplete{};

    // Reconnect plumbing (forwarded up to SessionArea)
    std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle;
    std::function<void(Session const*, SessionSnapshot)> requestReconnect;
    std::function<void(Session const*)> onReconnectFailed;
    std::function<void(Session const*)> onReconnectSucceeded;
    std::function<void(Session const*)> onReconnectCancel;
    std::function<void(Session const*)> onReconnectNow;
    std::function<void(bool success, std::string const& info)> onOpened;

    /**
     * @brief Sends a command to the last interacted terminal; tells the user when there is none or
     *        the connection is lost.
     */
    void runInTerminal(std::string const& command, bool execute)
    {
        if (isInLostConnectionState.value())
        {
            Notifications::warning(std::string{language->get("sessionFrontend", "disconnectedCannotRun")});
            return;
        }
        auto* const manager = frontendSessionManager.value().get();
        if (!manager || !manager->sendToLastInteracted(command, execute))
            Notifications::warning(std::string{language->get("sessionFrontend", "noTerminalToRunIn")});
    }

    explicit Implementation(Session::Params params)
        : stateHolder{params.stateHolder}
        , events{params.events}
        , confirmDialog{params.confirmDialog}
        , commandStoreClient{params.commandStoreClient}
        , initialName{std::move(params.initialName)}
        , persistenceName{params.isDirectConnect ? std::nullopt : std::optional<std::string>{this->initialName}}
        , tabTitle{std::make_shared<Nui::Observed<std::string>>(this->initialName)}
        , sessionLayoutId{Nui::val::global("generateId")().as<std::string>()}
        , closeSelf{std::move(params.closeSelf)}
        , isVisible{params.visible}
        , tabId{params.tabId}
        , uiOptions{params.uiOptions}
        , engineOptions{std::move(params.sessionOptions)}
        , layoutName{std::move(params.layoutName)}
        , fileExplorerPanel{FileExplorerPanel::Params{
            .stateHolder = params.stateHolder,
            .events = params.events,
            .confirmDialog = params.confirmDialog,
            .inputDialog = params.newItemAskDialog,
            .filePropertyDialog = params.filePropertyDialog,
            .archiveTransferDialog = params.archiveTransferDialog,
            .persistenceSessionName = this->persistenceName,
            .uiOptions = this->uiOptions,
            .engineOptions = this->engineOptions,
            .isInLostConnectionState = &this->isInLostConnectionState,
          }}
        , operationQueue{this->stateHolder, this->events, this->persistenceName, this->engineOptions.queueOptions.value(), this->confirmDialog, &fileExplorerPanel.localSideModel(), fileExplorerPanel.remoteSideModel()}
        , frontendSessionManager{}
        , sessionOptions{params.stateHolder, params.events, this->persistenceName, this->sessionLayoutId, params.confirmDialog}
        , fileTrackingPanel{FileTrackingPanel::Params{
              .confirmDialog = params.confirmDialog,
              .sessionLayoutId = this->sessionLayoutId,
          }}
        , commandHistoryPanel{
              params.commandStoreClient,
              params.events,
              params.confirmDialog,
              // Routes panel actions into this session's last interacted terminal. Captures the
              // Implementation, which outlives every mounted panel and never moves.
              [this](std::string const& command, bool execute)
              {
                  runInTerminal(command, execute);
              },
              &this->isInLostConnectionState}
        , commandSnippetsPanel{
              params.commandStoreClient,
              params.events,
              params.confirmDialog,
              params.snippetImportDialog,
              params.snippetPresetsDialog,
              [this](std::string const& command, bool execute)
              {
                  runInTerminal(command, execute);
              },
              &this->isInLostConnectionState}
        , syncDialog{params.confirmDialog, &this->operationQueue}
        , syncProgressDialog{}
        , disambiguateTitle{std::move(params.disambiguateTitle)}
        , requestReconnect{std::move(params.requestReconnect)}
        , onReconnectFailed{std::move(params.onReconnectFailed)}
        , onReconnectSucceeded{std::move(params.onReconnectSucceeded)}
        , onReconnectCancel{std::move(params.onReconnectCancel)}
        , onReconnectNow{std::move(params.onReconnectNow)}
        , onOpened{std::move(params.onOpened)}
    {
        // Recompare: rerun the whole scan-and-diff flow with the dialog's current
        // settings, reusing the cached local+remote roots.  The old provider is
        // replaced by a fresh one.
        syncDialog.setOnRecompareRequested(
            [this](SyncDialog::RecompareRequest req)
            {
                if (lastSyncLocalPath_.empty() || lastSyncRemotePath_.empty())
                    return;
                startSyncFlowImpl(lastSyncLocalPath_, lastSyncRemotePath_, req.scan, req.diffOptions);
            }
        );

        fileExplorerPanel.dropLayoutMetadata(sessionLayoutId);

        fileTrackingPanel.setRevealOnServer(
            [this](Paths::RemotePath const& path)
            {
                auto* const side = fileExplorerPanel.remoteFileGridSide();
                if (side == nullptr)
                    return;
                Nui::val::global("contentPanelManager").call<bool>("activateFileExplorer", sessionLayoutId);
                const auto parent = path.parent();
                const auto name = path.filename();
                if (!parent || !name)
                    return side->path(path.value());
                side->navigateAndSelect(parent->value(), name->value());
            }
        );
    }

    /**
     * @brief Runs one scan-and-diff flow: replaces any existing provider, drives
     *        the progress dialog through Listing → Comparing, then opens the
     *        sync dialog with the resulting summary.
     *
     *        Callers:
     *         - file-explorer "synchronize" click (initial open), and
     *         - sync-dialog "recompare" click (uses the cached paths).
     */
    void startSyncFlowImpl(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        SyncDialog::ScanSettings scan,
        SharedData::Sync::DiffOptions initialDiffOptions
    )
    {
        lastSyncLocalPath_ = localPath;
        lastSyncRemotePath_ = remotePath;

        // The progress dialog holds the provider while it scans and compares, then hands it to the sync dialog.
        syncProgressDialog.open(
            std::make_shared<BackendSyncProvider>(&operationQueue),
            localPath,
            remotePath,
            scan.respectIgnoreFiles,
            scan.recursive,
            scan.ignoreHidden,
            initialDiffOptions,
            [this, localPath, remotePath, scan](
                std::shared_ptr<BackendSyncProvider> provider, SharedData::Sync::DiffSnapshot snapshot
            )
            {
                syncDialog.open(std::move(provider), std::move(snapshot), localPath, remotePath, scan);
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        );
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

    /**
     * @brief Tells the user that the server @p host cannot check what an upload would replace, unless they asked
     *        never to be told again.
     */
    void announceUncheckedUploads(std::string const& host)
    {
        const auto consequence = [](std::string const& what, std::string const& why)
        {
            return ConfirmDialog::OpenOptions::ListElement{.text = fmt::format("{}: {}", what, why)};
        };
        confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("uploadDestinationCheck", "noticeHeader"),
            .text = fmt::format(fmt::runtime(language->get("uploadDestinationCheck", "noticeText")), host),
            .buttons = ConfirmDialog::Button::Ok,
            .listItems =
                {
                    consequence(
                        language->get("uploadDestinationCheck", "normalUploads"),
                        language->get("uploadDestinationCheck", "normalUploadsDescription")
                    ),
                    consequence(
                        language->get("uploadDestinationCheck", "unsafeFastUploads"),
                        language->get("uploadDestinationCheck", "unsafeFastUploadsDescription")
                    ),
                    consequence(
                        language->get("uploadDestinationCheck", "fileTrackingUploads"),
                        language->get("uploadDestinationCheck", "fileTrackingUploadsDescription")
                    ),
                },
            .neverShowAgainId = "uploadDestinationCheck",
        });
    }

    /**
     * @brief Asks whether the server of an FTP session can check what an upload would replace, marks the remote side
     *        when it cannot and tells the user when @p announce is set.
     */
    void checkUploadDestinations(FileEngine& fileEngine, bool announce)
    {
        fileEngine.capabilities(lifetime.guarded(
            [this, announce](std::optional<FileEngine::Capabilities> const& capabilities, std::string const& info)
            {
                if (!capabilities)
                {
                    Log::warn("Could not ask the server whether it can check upload destinations: {}", info);
                    return;
                }
                const bool uploadsNeedUnsafeFastMode = !capabilities->probeEntries;
                fileExplorerPanel.setUploadsNeedUnsafeFastMode(uploadsNeedUnsafeFastMode);
                auto const* ftp = std::get_if<Persistence::FtpSessionOptions>(&engineOptions.engine);
                if (uploadsNeedUnsafeFastMode && announce && ftp)
                    announceUncheckedUploads(ftp->host);
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        ));
    }

    /**
     * @brief Drops the replies of the backend that arrive after this session is gone. Last, so it goes first.
     */
    Utility::LifetimeToken lifetime{};
};

int Session::tabId() const
{
    return impl_->tabId;
}

std::string Session::layoutId() const
{
    return impl_->sessionLayoutId;
}

void Session::onDrop(
    bool isLocalSide,
    std::vector<SharedData::DroppedItem> entries,
    std::optional<std::string> const& subdir,
    NuiFileExplorer::DropAction suggested
)
{
    impl_->fileExplorerPanel.onDrop(isLocalSide, std::move(entries), subdir, suggested);
}

auto Session::makeChannelElement() -> Nui::ElementRenderer
{
    return impl_->terminalPanel->makeChannelElement();
}

bool Session::supportsLocalShell() const
{
    return std::holds_alternative<Persistence::SshSessionOptions>(impl_->engineOptions.engine) ||
        std::holds_alternative<Persistence::FtpSessionOptions>(impl_->engineOptions.engine);
}

std::optional<Session::LoginUser> Session::loginUser() const
{
    if (const auto* ssh = std::get_if<Persistence::SshSessionOptions>(&impl_->engineOptions.engine))
        return LoginUser{ssh->loginUser()};
    if (const auto* ftp = std::get_if<Persistence::FtpSessionOptions>(&impl_->engineOptions.engine))
        return LoginUser{ftp->loginUser()};
    return std::nullopt;
}

std::optional<std::string> Session::persistenceName() const
{
    return impl_->persistenceName;
}

Persistence::SessionOptions const& Session::engineOptions() const
{
    return impl_->engineOptions;
}

void Session::openLocalShellChannel(std::string const& shellName)
{
    if (!supportsLocalShell())
    {
        Log::warn("openLocalShellChannel called on a local session, ignored");
        return;
    }
    if (impl_->layoutInitializer)
        impl_->layoutInitializer->openLocalShellChannel(shellName);
}

auto Session::makeLocalShellChannelElement(std::string const& shellName) -> Nui::ElementRenderer
{
    return impl_->terminalPanel->makeLocalShellChannelElement(shellName);
}

auto Session::makeAdoptedLocalShellChannelElement(LocalShellAdoption adoption) -> Nui::ElementRenderer
{
    return impl_->terminalPanel->makeAdoptedLocalShellChannelElement(std::move(adoption));
}

NuiFileExplorer::Side* Session::remoteFileGridSide()
{
    return impl_->fileExplorerPanel.remoteFileGridSide();
}
NuiFileExplorer::Side& Session::localFileGridSide()
{
    return impl_->fileExplorerPanel.localFileGridSide();
}

auto Session::makeFileExplorerElement() -> Nui::ElementRenderer
{
    return impl_->fileExplorerPanel.makeFileExplorerElement();
}

Session::Session(Params params)
    : impl_{nullptr}
{
    // Pull the resume snapshot out of Params before Implementation consumes
    // the rest; it seeds the SessionSnapshotManager built below.
    auto initialResume = std::move(params.resumeFromSnapshot);
    impl_ = std::make_unique<Implementation>(std::move(params));

    impl_->connectionLossOverlay = std::make_unique<ConnectionLossOverlay>(ConnectionLossOverlay::Params{
        .sessionLayoutId = impl_->sessionLayoutId,
        .onReconnectClicked = [this]() { reconnect(); },
        .onReconnectNowClicked = [this]() {
            if (impl_->onReconnectNow)
                impl_->onReconnectNow(this);
        },
        .onReconnectCancelClicked = [this]() {
            if (impl_->onReconnectCancel)
                impl_->onReconnectCancel(this);
        },
    });

    // TerminalPanel must exist before the engine creators below so FSM
    // callbacks can bind to it.
    impl_->terminalPanel = std::make_unique<TerminalPanel>(TerminalPanel::Params{
        .stateHolder = impl_->stateHolder,
        .confirmDialog = impl_->confirmDialog,
        .frontendSessionManager = &impl_->frontendSessionManager,
        .engineOptions = &impl_->engineOptions,
        .sessionLayoutId = impl_->sessionLayoutId,
        .isInLostConnectionState = [this]() { return impl_->isInLostConnectionState.value(); },
        .onReconnectRequested = [this]() { reconnect(); },
        .onCloseSelfRequested = [this]() { closeSelf(); },
        .onChannelOpened = [this](std::optional<Ids::ChannelId> channelId, std::string const& info) {
            onOpenChannel(channelId, info);
        },
    });

    // Snapshot manager before layout initializer: the initializer consults it
    // for the pending layout and the local-shell adoption queue.
    impl_->snapshotManager = std::make_unique<SessionSnapshotManager>(SessionSnapshotManager::Params{
        .frontendSessionManager = &impl_->frontendSessionManager,
        .terminalPanel = impl_->terminalPanel.get(),
        .fileExplorerPanel = &impl_->fileExplorerPanel,
        .operationQueue = &impl_->operationQueue,
        .layoutInitializer = nullptr,
        .initialPending = std::move(initialResume),
    });

    impl_->layoutInitializer = std::make_unique<SessionLayoutInitializer>(SessionLayoutInitializer::Params{
        .stateHolder = impl_->stateHolder,
        .confirmDialog = impl_->confirmDialog,
        .sessionLayoutId = impl_->sessionLayoutId,
        .layoutName = impl_->layoutName,
        .engineOptions = &impl_->engineOptions,
        .frontendSessionManager = &impl_->frontendSessionManager,
        .isInLostConnectionState = &impl_->isInLostConnectionState,
        .terminalPanel = impl_->terminalPanel.get(),
        .fileExplorerPanel = &impl_->fileExplorerPanel,
        .operationQueue = &impl_->operationQueue,
        .fileTrackingPanel = &impl_->fileTrackingPanel,
        .sessionOptions = &impl_->sessionOptions,
        .commandHistoryPanel = &impl_->commandHistoryPanel,
        .commandSnippetsPanel = &impl_->commandSnippetsPanel,
        .pendingLocalShellAdoptions = impl_->snapshotManager->pendingLocalShellAdoptionsPtr(),
        .takeResumeLayout = [this]() -> std::optional<nlohmann::json> {
            return impl_->snapshotManager->takeResumeLayout();
        },
        .onLayoutCreationFailed = [this]() { closeSelf(); },
    });
    impl_->snapshotManager->setLayoutInitializer(impl_->layoutInitializer.get());

    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine))
    {
        createExecutingEngine();
        setupFileGrid();
    }
    else if (auto engine = makeRemoteTerminalEngine(
                 impl_->engineOptions,
                 [this]()
                 {
                     onTerminalConnectionLoss();
                 }
             ))
    {
        createRemoteEngine(std::move(engine));
        setupFileGrid();
    }
    else
    {
        Log::error("Unsupported frontendSessionManager engine type");
        return;
    }
}

Session::Session(Params params, std::unique_ptr<ProtoSession> proto)
    : impl_{nullptr}
{
    // Engine + UI options come from the proto; the caller's snapshot (already
    // enriched with ejected local-shell adoptions) takes precedence over the
    // proto's, so we only fall back to the proto when Params has none.
    params.sessionOptions = proto->takeSessionOptions();
    params.uiOptions = proto->takeUiOptions();
    if (!params.resumeFromSnapshot.has_value())
        params.resumeFromSnapshot = proto->takeResumeSnapshot();

    auto initialResume = std::move(params.resumeFromSnapshot);
    impl_ = std::make_unique<Implementation>(std::move(params));

    impl_->connectionLossOverlay = std::make_unique<ConnectionLossOverlay>(ConnectionLossOverlay::Params{
        .sessionLayoutId = impl_->sessionLayoutId,
        .onReconnectClicked = [this]() { reconnect(); },
        .onReconnectNowClicked = [this]() {
            if (impl_->onReconnectNow)
                impl_->onReconnectNow(this);
        },
        .onReconnectCancelClicked = [this]() {
            if (impl_->onReconnectCancel)
                impl_->onReconnectCancel(this);
        },
    });

    impl_->terminalPanel = std::make_unique<TerminalPanel>(TerminalPanel::Params{
        .stateHolder = impl_->stateHolder,
        .confirmDialog = impl_->confirmDialog,
        .frontendSessionManager = &impl_->frontendSessionManager,
        .engineOptions = &impl_->engineOptions,
        .sessionLayoutId = impl_->sessionLayoutId,
        .isInLostConnectionState = [this]() { return impl_->isInLostConnectionState.value(); },
        .onReconnectRequested = [this]() { reconnect(); },
        .onCloseSelfRequested = [this]() { closeSelf(); },
        .onChannelOpened = [this](std::optional<Ids::ChannelId> channelId, std::string const& info) {
            onOpenChannel(channelId, info);
        },
    });

    impl_->snapshotManager = std::make_unique<SessionSnapshotManager>(SessionSnapshotManager::Params{
        .frontendSessionManager = &impl_->frontendSessionManager,
        .terminalPanel = impl_->terminalPanel.get(),
        .fileExplorerPanel = &impl_->fileExplorerPanel,
        .operationQueue = &impl_->operationQueue,
        .layoutInitializer = nullptr,
        .initialPending = std::move(initialResume),
    });

    impl_->layoutInitializer = std::make_unique<SessionLayoutInitializer>(SessionLayoutInitializer::Params{
        .stateHolder = impl_->stateHolder,
        .confirmDialog = impl_->confirmDialog,
        .sessionLayoutId = impl_->sessionLayoutId,
        .layoutName = impl_->layoutName,
        .engineOptions = &impl_->engineOptions,
        .frontendSessionManager = &impl_->frontendSessionManager,
        .isInLostConnectionState = &impl_->isInLostConnectionState,
        .terminalPanel = impl_->terminalPanel.get(),
        .fileExplorerPanel = &impl_->fileExplorerPanel,
        .operationQueue = &impl_->operationQueue,
        .fileTrackingPanel = &impl_->fileTrackingPanel,
        .sessionOptions = &impl_->sessionOptions,
        .commandHistoryPanel = &impl_->commandHistoryPanel,
        .commandSnippetsPanel = &impl_->commandSnippetsPanel,
        .pendingLocalShellAdoptions = impl_->snapshotManager->pendingLocalShellAdoptionsPtr(),
        .takeResumeLayout = [this]() -> std::optional<nlohmann::json> {
            return impl_->snapshotManager->takeResumeLayout();
        },
        .onLayoutCreationFailed = [this]() { closeSelf(); },
    });
    impl_->snapshotManager->setLayoutInitializer(impl_->layoutInitializer.get());

    // Copy the pending Lumino layout out of the snapshot before onOpenSession
    // resets it, so the DOM-attach-triggered restore still finds the panels.
    if (impl_->snapshotManager->hasPending() &&
        !impl_->snapshotManager->pending().luminoLayout.is_null())
    {
        impl_->snapshotManager->seedPendingResumeLayout(
            impl_->snapshotManager->pending().luminoLayout
        );
    }

    // Move the already-opened FSM out of the proto and rebind its host-level
    // callbacks from the proto's no-ops onto this Session's handlers.
    impl_->frontendSessionManager = proto->takeFrontendSessionManager();
    impl_->frontendSessionManager.value()->setLockedUserInputHandler(
        [this](Ids::ChannelId channelId, std::string const& input) {
            impl_->terminalPanel->onLockedModeUserInput(channelId, input);
        }
    );
    if (auto* remoteSession = impl_->frontendSessionManager.value()->engine().remoteSession())
        remoteSession->setOnConnectionLoss([this]() { onTerminalConnectionLoss(); });

    // The proto only probed the transport, it never created a channel — so the capture wiring
    // arrives in time for every channel this session opens.
    wireCommandCapture();

    // Remote sessions are the only path through ProtoSession today, but build
    // the file grid regardless so the remote side is ready for snapshot application.
    setupFileGrid();

    // Transport is already up: drive the post-open success branch synchronously
    // rather than waiting on an open() callback.
    onOpenSession(EngineOpenResult::Opened, "adopted from proto-session");
}

std::optional<nlohmann::json> Session::getLayout() const
{
    if (!impl_->layoutInitializer)
        return std::nullopt;
    return impl_->layoutInitializer->getLayout();
}

void Session::setupFileGrid()
{
    impl_->fileExplorerPanel.setup();
    // Local deletes need no connection, their cards show in this queue from the start.
    localSideModel().operationQueue(&impl_->operationQueue);

    // Sync dialog + progress dialog stay on Session; wire them through the
    // panel's synchronize callback.  Every flow scans and diffs with the
    // settings the sync dialog currently shows.
    impl_->fileExplorerPanel.setOnSynchronize(
        [this](std::filesystem::path loc, std::filesystem::path rem)
        {
            const auto settings = impl_->syncDialog.currentSettings();
            impl_->startSyncFlowImpl(std::move(loc), std::move(rem), settings.scan, settings.diffOptions);
        }
    );
}

namespace
{
    /// What the history shows a command was run on: the ssh target, or the shell for local sessions.
    std::string makeHostLabel(Persistence::SessionOptions const& engineOptions)
    {
        if (const auto* ssh = std::get_if<Persistence::SshSessionOptions>(&engineOptions.engine))
        {
            const auto user = ssh->loginUser().name;
            return user ? fmt::format("{}@{}", *user, ssh->host) : ssh->host;
        }
        if (const auto* executing = std::get_if<Persistence::ExecutingSessionOptions>(&engineOptions.engine))
            return executing->command.string();
        return {};
    }
}

void Session::wireCommandCapture()
{
    if (!impl_->frontendSessionManager.value())
        return;

    const auto captureMode =
        impl_->engineOptions.historyOptions.value().captureMode.value_or(Persistence::HistoryCaptureMode::smart);

    auto* const manager = impl_->frontendSessionManager.value().get();
    manager->setCaptureMode(captureMode);

    if (captureMode == Persistence::HistoryCaptureMode::off || !impl_->commandStoreClient)
        return;

    manager->setOnCommandExecuted(
        [this, host = makeHostLabel(impl_->engineOptions)](
            Ids::ChannelId const&, std::optional<std::string> const& localShell, std::string const& command
        ) {
            impl_->commandStoreClient->recordExecution(localShell.value_or(host), command);
        }
    );
}

void Session::runInTerminal(std::string const& command, bool execute)
{
    impl_->runInTerminal(command, execute);
}

void Session::createRemoteEngine(std::unique_ptr<TerminalEngine> engine)
{
    Log::info("Creating {} engine", engine->engineName());

    // Eager-construct the aux ExecutingTerminalEngine so applySnapshot can
    // adopt ejected local-shell channels on a reconnect even before the user
    // opens an interactive local-shell tab.
    impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
        std::move(engine),
        [this](Ids::ChannelId channelId, std::string const& input) {
            impl_->terminalPanel->onLockedModeUserInput(channelId, input);
        },
        /*eagerAuxEngine=*/true
    );

    wireCommandCapture();

    impl_->frontendSessionManager.value()->open(
        [this](EngineOpenResult result, std::string const& info)
        {
            onOpenSession(result, info);
        }
    );
}

void Session::createExecutingEngine()
{
    Log::info("Creating executing engine");
    impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
        std::make_unique<ExecutingTerminalEngine>(ExecutingTerminalEngine::Settings{
            .onProcessChange =
                [this](Ids::ChannelId const& channelId, std::string const& cmdline)
            {
                Log::info("Tab title changed: {}", cmdline);
                *impl_->tabTitle = impl_->disambiguateTitle(this, cmdline);
                Nui::val::global("contentPanelManager")
                    .call<void>("renameTerminalById", impl_->sessionLayoutId, channelId.value(), cmdline);
                Nui::globalEventContext.executeActiveEventsImmediately();
            },
        }),
        [this](Ids::ChannelId channelId, std::string const& input) {
            impl_->terminalPanel->onLockedModeUserInput(channelId, input);
        }
    );

    wireCommandCapture();

    impl_->frontendSessionManager.value()->open(
        [this](EngineOpenResult result, std::string const& info)
        {
            onOpenSession(result, info);
        }
    );

    openLocalFilesystem();
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Session);

void Session::openLocalFilesystem()
{
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::getHome",
        [this](Nui::val response)
        {
            if (!response.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::getHome");
                return;
            }

            const auto success = response["success"].as<bool>();
            if (!success)
            {
                const auto error = response["error"].as<std::string>();
                Log::error("Failed to get home directory: {}", error);
                return;
            }

            if (!response.hasOwnProperty("path"))
            {
                Log::error("Invalid response from RpcFilesystem::getHome: missing 'path'");
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("sessionFrontend", "getHomeDirectoryFailed"),
                    .text = language->get("remoteSideModel", "invalidResponseFromBackend"),
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "getHomeDirectoryFailed",
                });
                return;
            }

            const auto homePath = response["path"].as<std::string>();
            localFileGridSide().path(homePath);
        }
    );
}

void Session::openSftp(std::string const& username, bool forceOpen)
{
    auto* const manager = impl_->frontendSessionManager.value().get();
    auto* const remoteSession = manager ? manager->engine().remoteSession() : nullptr;
    auto const* const transferOptions = transferOptionsOf(impl_->engineOptions);
    if (!remoteSession || !transferOptions)
    {
        Log::info("Cannot open a remote file system for a local session");
        return;
    }
    if (!forceOpen && !opensFileSystemByDefault(impl_->engineOptions))
        return;

    Log::info("Opening the remote file system");
    impl_->snapshotManager->setSftpIsOpen(true);
    auto fileEngine = std::make_shared<FileEngine>(remoteSession);

    if (!remoteSideModel())
    {
        Log::error(
            "Remote side model is not available, cannot open SFTP. This is a bug and should not happen by "
            "design."
        );
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "sftpInitializationFailed"),
            .text = language->get("sessionFrontend", "remoteSideModelUnavailable"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "sftpInitializationFailed",
        });
    }

    if (std::holds_alternative<Persistence::FtpSessionOptions>(impl_->engineOptions.engine))
    {
        // A reconnect only marks the remote side again, the user heard about it when the session started.
        const bool isResumed = impl_->snapshotManager->hasPending();
        impl_->checkUploadDestinations(*fileEngine, !isResumed);
    }

    if (remoteSideModel())
        remoteSideModel()->engine(fileEngine);
    localSideModel().engine(std::move(fileEngine));
    if (remoteSideModel())
        impl_->operationQueue.activate(remoteSideModel()->engine(), remoteSession->sessionId());
    impl_->fileTrackingPanel.activate(remoteSession->sessionId());
    localSideModel().operationQueue(&impl_->operationQueue);
    localSideModel().setFileTracking(&impl_->fileTrackingPanel);
    if (remoteSideModel())
    {
        remoteSideModel()->operationQueue(&impl_->operationQueue);
        remoteSideModel()->setFileTracking(&impl_->fileTrackingPanel);
        if (transferOptions->defaultDirectory)
        {
            remoteFileGridSide()->path(
                fmt::format(
                    fmt::runtime(transferOptions->defaultDirectory->generic_string()), fmt::arg("user", username)
                )
            );
        }
        remoteSideModel()->engine()->homeDirectory(
            [this, readUserDirectories = transferOptions->readUserDirectories.value_or(true)](
                std::optional<std::filesystem::path> const& home, std::string const& info
            )
            {
                if (!remoteSideModel())
                    return;
                if (!home)
                {
                    Log::error("Failed to determine the remote home directory: {}", info);
                    if (remoteFileGridSide()->path().empty())
                        remoteFileGridSide()->path("/");
                    return;
                }

                remoteSideModel()->setRemoteHome(*home, readUserDirectories);
                // A snapshot restore or the user may have navigated already.
                if (remoteFileGridSide()->path().empty())
                    remoteFileGridSide()->path(*home);
                if (auto* places = remoteFileGridSide()->places(); places)
                    places->reloadDefaultPlaces();
            }
        );
    }
    openLocalFilesystem();
}

void Session::announceRemoteUser(std::string const& user, std::string const& address)
{
    *impl_->tabTitle = impl_->disambiguateTitle(this, address);
    impl_->fileExplorerPanel.setRemoteOrigin(address);
    impl_->snapshotManager->setRemoteUsername(user);
}

void Session::resumeRemoteSession(std::string const& address)
{
    // Reconnect shortcut: reuse the username from the old Session to
    // avoid an "errorName" flash during RpcSystem::getUsername.
    const auto& snapshot = impl_->snapshotManager->pending();
    const auto& user = snapshot.remoteUsername;
    announceRemoteUser(user, address);
    if (snapshot.sftpWasOpen)
        openSftp(user, /*forceOpen=*/true);
    applySnapshot(snapshot);
    if (impl_->onReconnectSucceeded)
        impl_->onReconnectSucceeded(this);
    impl_->snapshotManager->resetPending();
}

void Session::onOpenSession(EngineOpenResult result, std::string const& info)
{
    const bool success = result == EngineOpenResult::Opened;
    if (impl_->onOpened)
        std::exchange(impl_->onOpened, {})(success, info);

    const bool reconnecting = impl_->snapshotManager && impl_->snapshotManager->hasPending();
    if (result == EngineOpenResult::LoginCancelled && !reconnecting)
    {
        closeSelf();
        return;
    }

    if (!success)
    {
        // Reconnect path: let SessionArea schedule the next retry.  It keeps
        // the snapshot, so no local preservation needed.
        if (impl_->snapshotManager && impl_->snapshotManager->hasPending() && impl_->onReconnectFailed)
        {
            Log::warn("Reconnect attempt failed: {}", info);
            impl_->onReconnectFailed(this);
            return;
        }

        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "sessionCreationFailedHeader"),
            .text = fmt::format(fmt::runtime(language->get("sessionFrontend", "sessionCreationFailedText")), language->translate(info)),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "sessionCreationFailed",
        });
        closeSelf();
        return;
    }

    Log::info("Session opened successfully: {}", info);
    const bool isResumed = impl_->snapshotManager && impl_->snapshotManager->hasPending();
    if (const auto* ssh = std::get_if<Persistence::SshSessionOptions>(&impl_->engineOptions.engine))
    {
        if (isResumed)
        {
            const auto& user = impl_->snapshotManager->pending().remoteUsername;
            resumeRemoteSession(remoteAddress(user, ssh->host, ssh->port.value_or(22)));
        }
        else
        {
            Log::info("Retrieving username for tab title");
            Nui::RpcClient::callWithBackChannel(
                "RpcSystem::getUsername",
                impl_->lifetime.guarded(
                    [this, ssh](Nui::val response)
                    {
                        std::string username = "errorName";
                        if (response.hasOwnProperty("username"))
                            username = response["username"].as<std::string>();
                        Log::info("Retrieved username: {}", username);

                        const auto user = ssh->loginUser().name.value_or(username);
                        announceRemoteUser(user, remoteAddress(user, ssh->host, ssh->port.value_or(22)));
                        openSftp(user);
                    }
                )
            );
        }
    }
    else if (const auto* ftp = std::get_if<Persistence::FtpSessionOptions>(&impl_->engineOptions.engine))
    {
        const auto user = ftpUserName(*ftp);
        const auto address = remoteAddress(user, ftp->host, ftp->effectivePort());
        if (isResumed)
        {
            resumeRemoteSession(address);
        }
        else
        {
            announceRemoteUser(user, address);
            openSftp(user);
        }
    }

    impl_->frontendSessionManager.value()->focusFirst();
    if (impl_->layoutInitializer)
        impl_->layoutInitializer->initialize();
}

void Session::onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info)
{
    if (!channelId)
    {
        Log::warn("Session::onOpenChannel: channel creation failed: {}.", info);
        impl_->terminalPanel->onChannelCreationFailed(info);
        return;
    }

    Log::info("Session::onOpenChannel: channel opened successfully: {}.", channelId->value());

    // Reconnect path: drain one scrollback dump into this channel iff it is
    // a primary (SSH) channel.  Local-shell adoption has its own replay path
    // (FrontendSessionManager::adoptChannel handles savedScrollback directly).
    if (impl_->snapshotManager && impl_->frontendSessionManager.value())
    {
        impl_->frontendSessionManager.value()->forEachChannel(
            FrontendSessionManager::EngineFilter::PrimaryOnly,
            [&channelId, this](Ids::ChannelId const& cid, TerminalChannel& channel) {
                if (cid != *channelId)
                    return true;
                impl_->snapshotManager->replayScrollbackFor(cid, channel);
                return false;
            }
        );
    }
}

void Session::onTerminalConnectionLoss()
{
    Log::debug("onTerminalConnectionLoss");
    if (!impl_->isInLostConnectionState.value())
        onConnectionLoss();
}

void Session::onConnectionLoss()
{
    if (impl_->isInLostConnectionState.value())
        return;

    impl_->isInLostConnectionState = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    if (!impl_->frontendSessionManager.value())
    {
        Log::error("Cannot write broadcast message, no frontend session manager");
        return;
    }

    // Only lock the primary (SSH) channels; local-shell channels are
    // transport-independent and must keep functioning.
    impl_->frontendSessionManager.value()->connectionLossMode(
        true, FrontendSessionManager::EngineFilter::PrimaryOnly
    );
    impl_->terminalPanel->captureChannelContentsForLockedMode();
    impl_->frontendSessionManager.value()->broadcast(
        language->get("sessionFrontend", "connectionLostTerminalMessage"),
        FrontendSessionManager::EngineFilter::PrimaryOnly
    );

    if (impl_->connectionLossOverlay)
        impl_->connectionLossOverlay->show();
}

void Session::shutdown(std::function<void()> onShutdown)
{
    impl_->onShutdownComplete = std::move(onShutdown);
    closeSelf();
}

void Session::closeSelf()
{
    Log::info("Session::closeSelf called");
    // Immediately make page inert to prevent user interaction from this point on.
    impl_->inertEverything = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    auto closeSelfCompletion = [this]()
    {
        Log::info("Removing session layout from content panel manager.");
        Nui::val::global("contentPanelManager").call<void>("removePanel", impl_->sessionLayoutId);

        // outside shutdown
        if (impl_->onShutdownComplete)
        {
            Log::info("Session shutdown complete.");
            impl_->onShutdownComplete();
            return;
        }
        else
            impl_->closeSelf(this);
    };

    // Dispose the FSM before removePanel for both engine types.  Otherwise
    // the deleter-triggered async channel-close chain can outlive the Session
    // and read freed memory in its RPC completion (UAF when the response for
    // ProcessStore::exit arrives after ~Session).
    if (impl_->frontendSessionManager.value())
    {
        Log::info("Session shutdown started.");
        impl_->operationQueue.deactivate();
        impl_->fileTrackingPanel.deactivate();
        impl_->frontendSessionManager.value()->dispose(
            [closeSelfCompletion]()
            {
                Log::info("Session.closeSelfCompletion()");
                closeSelfCompletion();
            }
        );
    }
    else
    {
        Log::info("Session shutdown is already complete (no frontend session manager).");
        closeSelfCompletion();
    }
}

std::optional<std::string> Session::getProcessIdIfExecutingEngine() const
{
    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine))
        return static_cast<ExecutingTerminalEngine&>(impl_->frontendSessionManager.value()->engine()).id();
    return std::nullopt;
}

bool Session::isInLostConnectionState() const
{
    return impl_->isInLostConnectionState.value();
}

SessionSnapshot Session::captureSnapshot(bool withEjection)
{
    return impl_->snapshotManager->capture(withEjection);
}

std::vector<LocalShellAdoption> Session::ejectLocalShellsForHandoff()
{
    return impl_->snapshotManager->ejectLocalShellsForHandoff();
}

void Session::applySnapshot(SessionSnapshot const& snapshot)
{
    impl_->snapshotManager->apply(snapshot);
}

void Session::reconnect()
{
    if (!impl_->isInLostConnectionState.value())
    {
        Log::warn("Session::reconnect called outside the lost-connection state, ignoring");
        return;
    }
    if (impl_->connectionLossOverlay && impl_->connectionLossOverlay->isReconnectCycleActive())
    {
        Log::warn("Session::reconnect called while a reconnect cycle is already active, ignoring");
        return;
    }
    if (!impl_->requestReconnect)
    {
        Log::error("Session::reconnect: no requestReconnect callback wired");
        return;
    }

    Log::info("Session::reconnect: capturing snapshot and handing off to SessionArea");
    // Non-destructive capture: the old Session stays alive while a ProtoSession
    // probes the transport.  Local-shell ejection happens at the swap moment
    // via ejectLocalShellsForHandoff, so cancelling keeps shells working.
    auto snapshot = captureSnapshot(/*withEjection=*/false);
    impl_->requestReconnect(this, std::move(snapshot));
}

void Session::startReconnectUi()
{
    if (impl_->connectionLossOverlay)
        impl_->connectionLossOverlay->startReconnectUi();
}

void Session::stopReconnectUi()
{
    if (impl_->connectionLossOverlay)
        impl_->connectionLossOverlay->stopReconnectUi();
}

void Session::setReconnectUiAttempt(int attempt)
{
    if (impl_->connectionLossOverlay)
        impl_->connectionLossOverlay->setReconnectUiAttempt(attempt);
}

void Session::setReconnectUiCountdown(int seconds)
{
    if (impl_->connectionLossOverlay)
        impl_->connectionLossOverlay->setReconnectUiCountdown(seconds);
}

std::optional<SessionSnapshot> Session::takePendingSnapshot()
{
    if (!impl_->snapshotManager)
        return std::nullopt;
    return impl_->snapshotManager->takePendingSnapshot();
}

std::string Session::name() const
{
    return impl_->initialName;
}

std::weak_ptr<Nui::Observed<std::string>> Session::tabTitle() const
{
    return impl_->tabTitle;
}

bool Session::visible() const
{
    return impl_->isVisible.value();
}

void Session::visible(bool value)
{
    impl_->isVisible = value;
    Nui::globalEventContext.executeActiveEventsImmediately();
    if (value)
        impl_->frontendSessionManager.value()->focusFirst();
}

void Session::onChannelClosedByUser(Ids::ChannelId const& channelId)
{
    impl_->terminalPanel->onChannelClosedByUser(channelId);
}


Nui::ElementRenderer Session::operator()()
{
    using Nui::Elements::div; // shadow the global div.
    using namespace Nui::Attributes;
    Log::info("Session::operator()");

    // clang-format off
    return div{
        class_ = observe(impl_->isVisible).generate([this]() {
            return classes("terminal-session", impl_->isVisible.value() ? "terminal-session-visible" : "terminal-session-hidden");
        }),
        !(reference = [this](
            std::weak_ptr<Nui::Dom::BasicElement>&& elem
        ){
            try {
                if (impl_->layoutInitializer)
                    impl_->layoutInitializer->attachLayoutHost(std::move(elem));
            } catch (const std::exception& e) {
                Log::error("Error while initializing layout in layout host: {}", e.what());
            }
        }),
        "inert"_attr = observe(impl_->inertEverything).generate([this]() -> std::optional<std::string> {
            return impl_->inertEverything.value() ? "true"s : std::optional<std::string>{std::nullopt};
        })
    }(
        impl_->layoutInitializer ? impl_->layoutInitializer->tabAddMenuRenderer() : Nui::nil(),
        impl_->syncDialog(),
        impl_->syncProgressDialog(),
        (*impl_->connectionLossOverlay)()
    );
    // clang-format on
}

RemoteSideModel* Session::remoteSideModel()
{
    return impl_->fileExplorerPanel.remoteSideModel();
}
LocalSideModel& Session::localSideModel()
{
    return impl_->fileExplorerPanel.localSideModel();
}