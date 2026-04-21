#include <persistence/state/session_options.hpp>
#include <frontend/session.hpp>
#include <frontend/proto_session.hpp>
#include <frontend/sync_dialog/sync_dialog.hpp>
#include <frontend/sync_dialog/sync_progress_dialog.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/session/session_helpers.hpp>
#include <frontend/icon_from_name.hpp>
#include <frontend/classes.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/session_components/session_options.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/session_components/file_tracking.hpp>
#include <frontend/session_components/terminal_panel.hpp>
#include <frontend/session_components/file_explorer_panel.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <nui-file-explorer/file_grid.hpp>
#include <persistence/state_holder.hpp>
#include <constants/layouts.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <script-nui-components/popup_menu.hpp>
#include <script-nui-components/button.hpp>
#include <script-nui-components/dialog.hpp>
#include <script-nui-components/anchored_panel.hpp>
#include <script-nui-components/spinner.hpp>

#include <ui5-sap-icons/icons/save.hpp>
#include <ui5-sap-icons/icons/copy.hpp>
#include <ui5-sap-icons/icons/document-text.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <nui/event_system/event_context.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/event_system/listen.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/delocalized.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/frontend/filesystem/file_dialog.hpp>

#include <algorithm>
#include <deque>
#include <ranges>
#include <unordered_map>
#include <vector>

using namespace Nui;
using namespace Nui::Elements;
using namespace Nui::Attributes;

struct Session::Implementation
{
    // Prop Drill:
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;

    // Session Ui Tab related:
    std::string initialName;
    std::shared_ptr<Nui::Observed<std::string>> tabTitle;
    std::string sessionLayoutId;
    std::function<void(Session const*)> closeSelf;
    Nui::Observed<bool> isVisible;
    int tabId;
    Persistence::UiOptions uiOptions;

    // Session Options Tab
    Persistence::SessionOptions engineOptions;

    // Add Tab Context Menu
    ScriptNuiComponents::PopupMenu tabAddMenu{};

    // Dialogs:
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;

    // Shared reactive flag: true while the SSH transport is considered lost.
    // Declared here (ahead of the widgets that observe it) so downstream
    // members can take a stable pointer during their own construction.
    Nui::Observed<bool> isInLostConnectionState{false};

    // File Explorer panel (grid + per-panel view blocker)
    FileExplorerPanel fileExplorerPanel;
    Nui::ListenRemover<Nui::Observed<std::shared_ptr<Nui::Dom::Element>>> fileExplorerListener{};

    // Operation Queue for File Explorer
    OperationQueue operationQueue;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> operationQueueElement;
    Nui::ListenRemover<decltype(operationQueueElement)> operationQueueListener{};

    // Layout Engine Related
    std::weak_ptr<Nui::Dom::BasicElement> layoutHost;
    std::optional<std::string> layoutName;
    bool waitingForLayoutHost{false};

    // Channels & FrontendSessionManager Connection
    Nui::Observed<std::unique_ptr<FrontendSessionManager>> frontendSessionManager;

    // Terminal panel (channels, toolbar, save/copy, locked-mode input)
    std::unique_ptr<TerminalPanel> terminalPanel;

    // Session Options
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> sessionOptionsElement{};
    Nui::ListenRemover<decltype(sessionOptionsElement)> sessionOptionsListener{};
    SessionOptions sessionOptions;

    // File Tracking
    FileTrackingPanel fileTrackingPanel;
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>> fileTrackingElement{};
    Nui::ListenRemover<decltype(fileTrackingElement)> fileTrackingListener{};

    // Sync Dialog
    SyncDialog syncDialog;
    SyncProgressDialog syncProgressDialog;

    // Shutdown & Connection Status:
    Nui::Observed<bool> inertEverything{false};
    std::function<void()> onShutdownComplete{};

    std::function<std::string(Session const* ptr, std::string const&)> disambiguateTitle;

    // Reconnect plumbing:
    std::function<void(Session const*, SessionSnapshot)> requestReconnect;
    std::function<void(Session const*)> onReconnectFailed;
    std::function<void(Session const*)> onReconnectSucceeded;
    std::function<void(Session const*)> onReconnectCancel;
    std::function<void(Session const*)> onReconnectNow;
    std::optional<SessionSnapshot> pendingResumeSnapshot;

    // In-session reconnect UI state (drives the lost-connection overlay).
    Nui::Observed<bool> reconnectCycleActive{false};
    Nui::Observed<int> reconnectAttempt{1};
    Nui::Observed<int> reconnectCountdown{0};
    /// Non-modal, draggable dialog that hosts the reconnect UI.  Opened on
    /// connection loss, closed on Session disposal.  The draggability lets
    /// the user move it out of the way to keep working with local-shell
    /// panels while the SSH transport tries to reconnect.
    std::unique_ptr<ScriptNuiComponents::Dialog> connectionLostDialog;
    /**
     * @brief Bulk OperationIds whose backend resume backups this Session is
     *        responsible for cleaning up.  Populated from the snapshot's
     *        bulk ResumableOps in applySnapshot — the destructor fires
     *        SessionManager::discardBulkResume for each, so abandoned
     *        backups don't sit in the registry until TTL eviction.  Adopt
     *        already removes consumed entries server-side, so the discard
     *        is a no-op in the happy path.
     */
    std::vector<Ids::OperationId> trackedBulkResumes;
    /**
     * @brief Populated from pendingResumeSnapshot.ejectedLocalShells just
     *        before initializeLayout runs.  Each Lumino localShellFactory
     *        invocation consumes the first entry whose shellConfigName
     *        matches, adopting its backend process instead of spawning a
     *        fresh one.  Unmatched entries at the end of layout restore are
     *        dropped (the saved layout didn't have tabs for them).
     */
    std::vector<LocalShellAdoption> pendingLocalShellAdoptions;

    /**
     * @brief Scrollback dumps waiting to be replayed into the next opened
     *        primary channel(s).  applySnapshot seeds this from the snapshot
     *        because the channels don't yet exist at snapshot-apply time
     *        (initializeLayout builds them later via the terminalFactory,
     *        and each channel only gets a live xterm id inside its own
     *        onMaterialize).  onOpenChannel pops the front entry and feeds
     *        it to TerminalChannel::replayContent.
     */
    std::deque<std::string> pendingScrollbackReplay;

    /**
     * @brief Lumino layout waiting to be applied by initializeLayout.  Split
     *        out of pendingResumeSnapshot so the adoption path (which runs
     *        onOpenSession synchronously in the constructor, before the DOM
     *        attaches) can still drive initializeLayout after the snapshot
     *        has been consumed and reset.
     */
    nlohmann::json pendingResumeLayout{};

    /**
     * @brief Most recently retrieved remote username (from RpcSystem::getUsername
     *        during onOpenSession).  Preserved into the snapshot so the
     *        reconnect path can skip re-fetching + re-render the tab title
     *        with the same user before async resolution finishes.
     */
    std::string remoteUsername;
    bool sftpIsOpen{false};

    /**
     * @brief Rebuilds the "+" dock context menu's entries.
     *
     * Called from the constructor (initial build) and each time the user opens
     * the menu (so the Local Shell list reflects the current set of saved shell
     * sessions and the disabled state of single-instance panels is correct).
     */
    void rebuildTabAddMenu();

    explicit Implementation(Session::Params params)
        : stateHolder{params.stateHolder}
        , events{params.events}
        , initialName{std::move(params.initialName)}
        , tabTitle{std::make_shared<Nui::Observed<std::string>>(this->initialName)}
        , sessionLayoutId{Nui::val::global("generateId")().as<std::string>()}
        , closeSelf{std::move(params.closeSelf)}
        , isVisible{params.visible}
        , tabId{params.tabId}
        , uiOptions{params.uiOptions}
        , engineOptions{std::move(params.sessionOptions)}
        , inputDialog{params.newItemAskDialog}
        , confirmDialog{params.confirmDialog}
        , fileExplorerPanel{FileExplorerPanel::Params{
            .stateHolder = params.stateHolder,
            .events = params.events,
            .confirmDialog = params.confirmDialog,
            .inputDialog = params.newItemAskDialog,
            .filePropertyDialog = params.filePropertyDialog,
            .archiveTransferDialog = params.archiveTransferDialog,
            .sessionName = this->initialName,
            .uiOptions = this->uiOptions,
            .engineOptions = this->engineOptions,
            .isInLostConnectionState = &this->isInLostConnectionState,
          }}
        , operationQueue{this->stateHolder, this->events, this->initialName, this->confirmDialog, &fileExplorerPanel.localSideModel(), fileExplorerPanel.remoteSideModel()}
        , operationQueueElement{}
        , layoutHost{}
        , layoutName{std::move(params.layoutName)}
        , frontendSessionManager{}
        , sessionOptionsElement{}
        , sessionOptions{params.stateHolder, params.events, this->initialName, this->sessionLayoutId, params.confirmDialog}
        , fileTrackingPanel{params.stateHolder, params.events, params.confirmDialog}
        , fileTrackingElement{}
        , syncDialog{params.confirmDialog, &this->operationQueue}
        , syncProgressDialog{&this->operationQueue}
        , disambiguateTitle{std::move(params.disambiguateTitle)}
        , requestReconnect{std::move(params.requestReconnect)}
        , onReconnectFailed{std::move(params.onReconnectFailed)}
        , onReconnectSucceeded{std::move(params.onReconnectSucceeded)}
        , onReconnectCancel{std::move(params.onReconnectCancel)}
        , onReconnectNow{std::move(params.onReconnectNow)}
        , pendingResumeSnapshot{std::move(params.resumeFromSnapshot)}
    {
        // Seed local-shell adoptions from the snapshot up front: the Lumino
        // layout restore (kicked off by initializeLayout once the DOM host
        // attaches) can race ahead of onOpenSession's applySnapshot, and if
        // pendingLocalShellAdoptions is still empty when localShellFactory
        // fires, every saved shell tab respawns as a fresh process instead
        // of adopting its ejected backend process.
        if (pendingResumeSnapshot.has_value())
            pendingLocalShellAdoptions = pendingResumeSnapshot->ejectedLocalShells;
        syncDialog.setOnRecompare(
            [this](
                std::filesystem::path loc,
                std::filesystem::path rem,
                bool respectIgnoreFiles,
                bool recursive,
                bool ignoreHidden,
                std::function<void(
                    std::vector<SharedData::DirectoryEntry>,
                    std::vector<SharedData::DirectoryEntry>
                )> onResult
            )
            {
                syncProgressDialog.open(
                    std::move(loc), std::move(rem), respectIgnoreFiles, recursive, ignoreHidden, std::move(onResult)
                );
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        );

        fileExplorerPanel.dropLayoutMetadata(sessionLayoutId);

        using namespace ScriptNuiComponents;
        using namespace std::string_literals;

        rebuildTabAddMenu();

        sessionOptionsListener = Nui::smartListen(
            sessionOptionsElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                tabAddMenu.modifyItemByLabel(
                    language->get("sessionFrontend", "sessionOptions"),
                    [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                    {
                        if (mi)
                            mi->disabled = dis;
                    }
                );
            }
        );
        fileExplorerListener = Nui::smartListen(
            fileExplorerPanel.elementObservable(),
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                Nui::WebApi::Console::log("fileExplorerElement changed.");
                tabAddMenu.modifyItemByLabel(
                    language->get("sessionFrontend", "fileExplorer"),
                    [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                    {
                        if (mi)
                        {
                            Nui::WebApi::Console::log("Menu item found, modifying disabled to " + std::to_string(dis));
                            mi->disabled = dis;
                        }
                    }
                );
            }
        );
        operationQueueListener = Nui::smartListen(
            operationQueueElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                Nui::WebApi::Console::log("operationQueueElement changed.");

                tabAddMenu.modifyItemByLabel(
                    language->get("sessionFrontend", "operationQueue"),
                    [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                    {
                        if (mi)
                            mi->disabled = dis;
                    }
                );
            }
        );
        fileTrackingListener = Nui::smartListen(
            fileTrackingElement,
            [this](std::shared_ptr<Nui::Dom::Element> const& elem)
            {
                tabAddMenu.modifyItemByLabel(
                    language->get("sessionFrontend", "fileTracking"),
                    [dis = elem != nullptr](ScriptNuiComponents::PopupMenu::MenuItem* mi)
                    {
                        if (mi)
                            mi->disabled = dis;
                    }
                );
            }
        );
    }
};

void Session::Implementation::rebuildTabAddMenu()
{
    using namespace ScriptNuiComponents;
    using namespace std::string_literals;

    const bool isSsh = std::holds_alternative<Persistence::SshSessionOptions>(engineOptions.engine);

    std::vector<PopupMenu::Entry> items;
    items.reserve(8);

    items.push_back(PopupMenu::sectionHeader(language->get("sessionFrontend", "newTab")));

    items.push_back(PopupMenu::item(
        language->get("sessionFrontend", "terminal"),
        std::string{},
        [this]()
        {
            Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "terminal"s);
            tabAddMenu.close();
        }
    ));

    items.push_back(PopupMenu::item(
        language->get("sessionFrontend", "fileExplorer"),
        std::string{},
        [this]()
        {
            tabAddMenu.close();
            if (fileExplorerPanel.elementObservable().value())
                return;
            Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-explorer"s);
        },
        fileExplorerPanel.elementObservable().value() != nullptr
    ));

    if (isSsh)
    {
        items.push_back(PopupMenu::item(
            language->get("sessionFrontend", "operationQueue"),
            std::string{},
            [this]()
            {
                tabAddMenu.close();
                if (operationQueueElement.value())
                    return;
                Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "operation-queue"s);
            },
            operationQueueElement.value() != nullptr
        ));
        items.push_back(PopupMenu::item(
            language->get("sessionFrontend", "fileTracking"),
            std::string{},
            [this]()
            {
                tabAddMenu.close();
                if (fileTrackingElement.value())
                    return;
                Nui::val::global("contentPanelManager").call<void>("fullfillLastAddRequest", "file-tracking"s);
            },
            fileTrackingElement.value() != nullptr
        ));

        // Dynamic Local Shell section: one entry per saved shell-type SessionOptions.
        std::vector<std::pair<std::string, std::string>> shells; // (name, icon)
        for (auto const& [name, sess] : stateHolder->stateCache().sessions)
        {
            if (sess.type == Persistence::TerminalEngineType::shell &&
                std::holds_alternative<Persistence::ExecutingSessionOptions>(sess.engine))
            {
                shells.emplace_back(name, sess.icon);
            }
        }

        items.push_back(PopupMenu::separator());
        items.push_back(PopupMenu::sectionHeader(language->get("sessionFrontend", "localShell")));

        if (shells.empty())
        {
            items.push_back(PopupMenu::item(
                language->get("sessionFrontend", "noLocalShellsConfigured"),
                std::string{},
                []() {},
                /*disabled=*/true,
                std::string{},
                language->get("sessionFrontend", "noLocalShellsTooltip")
            ));
        }
        else
        {
            for (auto const& [name, icon] : shells)
            {
                items.push_back(PopupMenu::item(
                    name,
                    icon.empty() ? Nui::nil() : iconFromName(icon),
                    [this, name]()
                    {
                        tabAddMenu.close();
                        Nui::val::global("contentPanelManager")
                            .call<void>("fullfillLastAddRequest", "local-shell:"s + name);
                    }
                ));
            }
        }
    }

    tabAddMenu.setItems(std::move(items));
}

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
    std::vector<SharedData::DirectoryEntry> entries,
    std::optional<std::string> const& subdir
)
{
    impl_->fileExplorerPanel.onDrop(isLocalSide, std::move(entries), subdir);
}

auto Session::makeChannelElement() -> Nui::ElementRenderer
{
    return impl_->terminalPanel->makeChannelElement();
}

bool Session::supportsLocalShell() const
{
    return std::holds_alternative<Persistence::SshSessionOptions>(impl_->engineOptions.engine);
}

void Session::openLocalShellChannel(std::string const& shellName)
{
    using namespace std::string_literals;

    if (!supportsLocalShell())
    {
        Log::warn("openLocalShellChannel called on non-SSH session — ignored");
        return;
    }

    // The layout id carries the shell name so saved layouts round-trip and
    // content_panel_manager can route factory calls to the right shell config.
    Nui::val::global("contentPanelManager")
        .call<void>("fullfillLastAddRequest", "local-shell:"s + shellName);
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

Nui::ElementRenderer Session::makeConnectionLostDialogBody()
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return div{class_ = "session-reconnect-panel-body"}(
            observe(impl_->reconnectCycleActive),
            [this]() -> Nui::ElementRenderer {
                using Nui::Elements::div;
                using Nui::Elements::span;
                using namespace Nui::Attributes;
                namespace Snc = ScriptNuiComponents;

                if (!impl_->reconnectCycleActive.value())
                {
                    return Snc::button({
                        .text = language->getObserved("sessionFrontend", "reconnectButton"),
                        .attributes = {onClick = [this]() { reconnect(); }},
                        .styleVariant = Snc::StyleVariant::Primary,
                    });
                }

                // Cycle UI: spinner + attempt + countdown + [Now] + [Cancel].
                // Wired to Params::onReconnectNow / onReconnectCancel — the
                // retry timers and the candidate ProtoSession live in
                // SessionArea, so this widget's only job is to hand user
                // intent upward.
                return div{class_ = "session-reconnect-cycle"}(
                    div{class_ = "session-reconnect-cycle-row"}(
                        Snc::spinner({.size = "22px", .thickness = "3px", .color = std::nullopt}),
                        span{}(
                            observe(impl_->reconnectAttempt),
                            [this]() {
                                return fmt::format(
                                    fmt::runtime(language->get("reconnectDialog", "attempt")),
                                    impl_->reconnectAttempt.value()
                                );
                            }
                        )
                    ),
                    div{class_ = "session-reconnect-cycle-row"}(
                        observe(impl_->reconnectCountdown),
                        [this]() -> Nui::ElementRenderer {
                            if (impl_->reconnectCountdown.value() <= 0)
                                return span{}(language->getObserved("reconnectDialog", "restoringState"));
                            return span{}(
                                fmt::format(
                                    fmt::runtime(language->get("reconnectDialog", "retryIn")),
                                    impl_->reconnectCountdown.value()
                                )
                            );
                        }
                    ),
                    div{class_ = "session-reconnect-cycle-buttons"}(
                        observe(impl_->reconnectCountdown),
                        [this]() -> Nui::ElementRenderer {
                            using Nui::Elements::div;
                            using namespace Nui::Attributes;
                            namespace Snc = ScriptNuiComponents;
                            // Hide [Now] while the retry is already firing
                            // (countdown == 0 → "Restoring..." state) — the
                            // click would be a no-op there.
                            auto nowButton = (impl_->reconnectCountdown.value() > 0)
                                ? Snc::button({
                                      .text = language->getObserved("reconnectDialog", "now"),
                                      .attributes = {onClick = [this]() {
                                          if (impl_->onReconnectNow)
                                              impl_->onReconnectNow(this);
                                      }},
                                      .styleVariant = Snc::StyleVariant::Primary,
                                  })
                                : Nui::ElementRenderer{Nui::nil()};
                            return div{class_ = "session-reconnect-cycle-buttons-inner"}(
                                std::move(nowButton),
                                Snc::button({
                                    .text = language->getObserved("reconnectDialog", "cancel"),
                                    .attributes = {onClick = [this]() {
                                        if (impl_->onReconnectCancel)
                                            impl_->onReconnectCancel(this);
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

auto Session::makeFileExplorerElement() -> Nui::ElementRenderer
{
    return impl_->fileExplorerPanel.makeFileExplorerElement();
}

Session::Session(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{
    // Build the per-session reconnect dialog before any connection-loss
    // handlers fire so onConnectionLoss can safely call open() on it.
    impl_->connectionLostDialog = std::make_unique<ScriptNuiComponents::Dialog>(
        impl_->sessionLayoutId + "-reconnect-dialog",
        makeConnectionLostDialogBody()
    );

    // Terminal panel owns terminal element factories, toolbar action handlers,
    // locked-mode input handling, and per-local-shell metadata.  Must exist
    // before the engine creators below so FSM callbacks can bind to it.
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

    if (std::holds_alternative<Persistence::ExecutingSessionOptions>(impl_->engineOptions.engine))
    {
        createExecutingEngine();
        setupFileGrid();
    }
    else if (std::holds_alternative<Persistence::SshSessionOptions>(impl_->engineOptions.engine))
    {
        createSshEngine();
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
    // Engine + UI options always come from the proto — that's the whole
    // reason the proto exists.  The resume snapshot, however, is only
    // pulled from the proto when the caller didn't already supply one:
    // SessionArea's ProtoSession flow captures a non-destructive snapshot
    // at Reconnect click, enriches it with ejected local-shell adoptions
    // at swap time, and hands it through Params.  Using the proto's copy
    // would skip the enrichment step.
    params.sessionOptions = proto->takeSessionOptions();
    params.uiOptions = proto->takeUiOptions();
    if (!params.resumeFromSnapshot.has_value())
        params.resumeFromSnapshot = proto->takeResumeSnapshot();

    impl_ = std::make_unique<Implementation>(std::move(params));

    // Same lifecycle as the fresh ctor — build the reconnect dialog before
    // onOpenSession-adoption side effects reach any handler that might
    // call open() on it.
    impl_->connectionLostDialog = std::make_unique<ScriptNuiComponents::Dialog>(
        impl_->sessionLayoutId + "-reconnect-dialog",
        makeConnectionLostDialogBody()
    );

    // Same construction as the fresh ctor — build the terminal panel before
    // the FSM's locked-user-input handler rebind below binds into it.
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

    // Extract the Lumino layout out of the snapshot before onOpenSession
    // resets it; initializeLayout reads from pendingResumeLayout first so
    // the DOM-attach-triggered restore still finds the saved panels.
    if (impl_->pendingResumeSnapshot && !impl_->pendingResumeSnapshot->luminoLayout.is_null())
        impl_->pendingResumeLayout = impl_->pendingResumeSnapshot->luminoLayout;

    // Move the already-opened FSM out of the proto and rebind its host-level
    // callbacks from the proto's no-ops onto this Session's handlers.
    impl_->frontendSessionManager = proto->takeFrontendSessionManager();
    impl_->frontendSessionManager.value()->setLockedUserInputHandler(
        [this](Ids::ChannelId channelId, std::string const& input) {
            impl_->terminalPanel->onLockedModeUserInput(channelId, input);
        }
    );
    if (impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
    {
        auto* sshEngine =
            static_cast<SshTerminalEngine*>(&impl_->frontendSessionManager.value()->engine());
        sshEngine->setOnConnectionLoss([this]() { onTerminalConnectionLoss(); });
    }

    // SSH is the only path that goes through ProtoSession today; local shells
    // still use the fresh constructor.  We still build the file grid so the
    // remote side is ready when the snapshot's file-explorer state is applied.
    setupFileGrid();

    // Drive the post-open success branch synchronously.  The transport is
    // already up, so simulating onOpenSession(true, …) here is the shortest
    // path to re-establishing the file grid, SFTP channel, scrollback replay
    // queue, and tab title — everything the fresh path gets via the open()
    // callback.  onOpenSession clears pendingResumeSnapshot as part of its
    // work; pendingResumeLayout (saved above) takes over for the later
    // DOM-attach-triggered initializeLayout.
    onOpenSession(true, "adopted from proto-session");

    // `proto` goes out of scope here and is destroyed — it's now an empty
    // shell with no FSM, no snapshot, nothing useful.
}

std::optional<nlohmann::json> Session::getLayout() const
{
    // session layout id is not the name in the setting, but the id for the lumino datastructure in the
    // contentPanelManager where this session lives in.
    auto layout = Nui::val::global("contentPanelManager").call<Nui::val>("getPanelLayout", impl_->sessionLayoutId);
    if (layout.isUndefined())
        return std::nullopt;
    auto layoutObject = nlohmann::json::parse(Nui::JSON::stringify(layout));
    layoutObject["__extra"] = {{
        "fileGrid",
        {
            {
                "leftSide",
                {
                    {"flavor", fileGridFlavorToString(impl_->fileExplorerPanel.localFileGridSide().flavor())},
                },
            },
        },
    }};
    if (impl_->fileExplorerPanel.remoteFileGridSide())
    {
        layoutObject["__extra"]["fileGrid"]["rightSide"] = {
            {"flavor", fileGridFlavorToString(impl_->fileExplorerPanel.remoteFileGridSide()->flavor())},
        };
    }
    return layoutObject;
}

void Session::setupFileGrid()
{
    impl_->fileExplorerPanel.setup();

    // The sync dialog + progress dialog stay on Session; wire them in through
    // the panel's setter so the grid's synchronize callback opens them.
    impl_->fileExplorerPanel.setOnSynchronize(
        [this](std::filesystem::path loc, std::filesystem::path rem)
        {
            impl_->syncProgressDialog.open(
                loc,
                rem,
                true,
                true,
                false,
                [this, loc, rem](auto localEntries, auto remoteEntries)
                {
                    impl_->syncDialog.open(loc, rem, std::move(localEntries), std::move(remoteEntries));
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void Session::createSshEngine()
{
    Log::info("Creating SSH engine");

    // Eager-construct the aux ExecutingTerminalEngine so Session::applySnapshot
    // can adopt ejected local-shell channels on a reconnect even before the
    // user opens an interactive local-shell tab.  Cheap — open() returns
    // immediately.
    impl_->frontendSessionManager = std::make_unique<FrontendSessionManager>(
        std::make_unique<SshTerminalEngine>(SshTerminalEngine::Settings{
            .sessionOptions = impl_->engineOptions,
            .onConnectionLoss = std::bind(&Session::onTerminalConnectionLoss, this),
        }),
        [this](Ids::ChannelId channelId, std::string const& input) {
            impl_->terminalPanel->onLockedModeUserInput(channelId, input);
        },
        /*eagerAuxEngine=*/true
    );

    impl_->frontendSessionManager.value()->open(
        std::bind(&Session::onOpenSession, this, std::placeholders::_1, std::placeholders::_2)
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

    impl_->frontendSessionManager.value()->open(
        std::bind(&Session::onOpenSession, this, std::placeholders::_1, std::placeholders::_2)
    );

    openLocalFilesystem();
}

Session::~Session()
{
    // Discard any backend resume backups this session is responsible for.
    // Adopt already removes consumed entries server-side, so this is a
    // no-op in the happy path; it matters when the user closes the tab
    // without ever reaching applySnapshot, or when applySnapshot ran but
    // adoptBulkResume failed and left entries in the registry.
    if (!impl_->trackedBulkResumes.empty())
    {
        Nui::val opIds = Nui::val::array();
        std::size_t idx = 0;
        for (auto const& opId : impl_->trackedBulkResumes)
            opIds.set(idx++, opId.value());
        Nui::val args = Nui::val::object();
        args.set("operationIds", opIds);
        Nui::RpcClient::callWithBackChannel(
            "SessionManager::discardBulkResume",
            [count = impl_->trackedBulkResumes.size()](Nui::val val) {
                if (val.hasOwnProperty("error"))
                    Log::warn("discardBulkResume failed for {} id(s): {}", count, val["error"].as<std::string>());
            },
            args
        );
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(Session);

void Session::openLocalFilesystem()
{
    // initial navigate to default path:
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
                    .headerText = "Get Home Directory Failed",
                    .text = "Invalid response from backend: missing 'path'",
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
    if (impl_->frontendSessionManager.value() && impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
    {
        auto const& opts = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);
        if (forceOpen || opts.openSftpByDefault)
        {
            Log::info("Opening SFTP by default");
            impl_->sftpIsOpen = true;
            auto* sshTerminalEngine = static_cast<SshTerminalEngine*>(&impl_->frontendSessionManager.value()->engine());
            auto fileEngine = std::make_shared<FileEngine>(sshTerminalEngine);

            if (!remoteSideModel())
            {
                Log::error(
                    "Remote side model is not available, cannot open SFTP. This is a bug and should not happen by "
                    "design."
                );
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "SFTP Initialization Failed",
                    .text = "Remote side model is not available, cannot open SFTP. This is a bug and should not happen "
                            "by design.",
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "sftpInitializationFailed",
                });
            }

            if (remoteSideModel())
                remoteSideModel()->engine(fileEngine);
            localSideModel().engine(std::move(fileEngine));
            if (remoteSideModel())
                impl_->operationQueue.activate(remoteSideModel()->engine(), sshTerminalEngine->sshSessionId());
            impl_->fileTrackingPanel.activate(&impl_->operationQueue, sshTerminalEngine->sshSessionId());
            localSideModel().operationQueue(&impl_->operationQueue);
            if (remoteSideModel())
            {
                remoteSideModel()->operationQueue(&impl_->operationQueue);
                remoteSideModel()->setFileTracking(&impl_->fileTrackingPanel);
                remoteSideModel()->setRemoteUsername(username);
                remoteFileGridSide()->path(
                    fmt::format(
                        fmt::runtime(opts.sftpOptions->defaultDirectory.value_or("/home/{user}").generic_string()),
                        fmt::arg("user", username)
                    )
                );
                if (auto* places = remoteFileGridSide()->places(); places)
                    places->reloadDefaultPlaces();
            }
            openLocalFilesystem();
        }
    }
    else
    {
        Log::info("Cannot open SFTP for non-ssh terminal");
    }
}

void Session::onOpenSession(bool success, std::string const& info)
{
    if (!success)
    {
        // Reconnect path: don't show a dialog or close the tab — signal
        // SessionArea to schedule the next retry.  SessionArea keeps the
        // snapshot, so we don't need to preserve anything locally.
        if (impl_->pendingResumeSnapshot.has_value() && impl_->onReconnectFailed)
        {
            Log::warn("Reconnect attempt failed: {}", info);
            impl_->onReconnectFailed(this);
            return;
        }

        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "sessionCreationFailedHeader"),
            .text = fmt::format(fmt::runtime(language->get("sessionFrontend", "sessionCreationFailedText")), info),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "sessionCreationFailed",
        });
        closeSelf();
        return;
    }
    else
    {
        Log::info("Session opened successfully: {}", info);
        if (impl_->frontendSessionManager.value() &&
            impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
        {
            const auto& sshSessionOptions = std::get<Persistence::SshSessionOptions>(impl_->engineOptions.engine);

            // Reconnect shortcut: the old Session already resolved the username.
            // Rewriting the tab title + opening SFTP synchronously avoids a
            // visible "errorName" flash during the RpcSystem::getUsername round
            // trip.  We still fire the RPC on fresh opens.
            if (impl_->pendingResumeSnapshot.has_value())
            {
                const auto& snapshot = *impl_->pendingResumeSnapshot;
                const auto& user = snapshot.remoteUsername;
                auto host = sshSessionOptions.host;
                const auto port = sshSessionOptions.port.value_or(22);
                if (host.find(":") != std::string::npos)
                    host = "[" + host + "]";
                *impl_->tabTitle = impl_->disambiguateTitle(this, fmt::format("{}@{}:{}", user, host, port));
                impl_->remoteUsername = user;
                if (snapshot.sftpWasOpen)
                    openSftp(user, /*forceOpen=*/true);
                applySnapshot(snapshot);
                if (impl_->onReconnectSucceeded)
                    impl_->onReconnectSucceeded(this);
                impl_->pendingResumeSnapshot.reset();
                impl_->frontendSessionManager.value()->focusFirst();
                initializeLayout();
                return;
            }

            Log::info("Retrieving username for tab title");
            Nui::RpcClient::callWithBackChannel(
                "RpcSystem::getUsername",
                [this, &sshSessionOptions](Nui::val response)
                {
                    std::string username = "errorName";
                    if (response.hasOwnProperty("username"))
                        username = response["username"].as<std::string>();
                    Log::info("Retrieved username: {}", username);

                    const auto user = sshSessionOptions.user.value_or(username);
                    auto host = sshSessionOptions.host;
                    const auto port = sshSessionOptions.port.value_or(22);

                    // assume ipv6 when finding ':' in host
                    if (host.find(":") != std::string::npos)
                        host = "[" + host + "]";
                    *impl_->tabTitle = impl_->disambiguateTitle(this, fmt::format("{}@{}:{}", user, host, port));
                    impl_->remoteUsername = user;

                    openSftp(user);
                }
            );
        }

        impl_->frontendSessionManager.value()->focusFirst();
        initializeLayout();
    }
}

void Session::onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info)
{
    if (!channelId)
    {
        impl_->terminalPanel->onChannelCreationFailed(info);
        return;
    }

    Log::info("Channel opened successfully: {}", channelId->value());

    // Reconnect path: drain one scrollback dump into this channel iff it is
    // a primary (SSH) channel.  applySnapshot populates the queue before the
    // layout restore builds any channels, so we can't replay at snapshot-apply
    // time.  The queue is consumed in the same order primary channels finish
    // opening, which matches the FSM iteration order captureSnapshot used.
    // Local-shell adoption has its own replay path (FrontendSessionManager::
    // adoptChannel handles savedScrollback directly), so we must not pop on
    // their onOpenChannel or a primary's dump would get sent to a shell.
    if (!impl_->pendingScrollbackReplay.empty() && impl_->frontendSessionManager.value())
    {
        impl_->frontendSessionManager.value()->forEachChannel(
            FrontendSessionManager::EngineFilter::PrimaryOnly,
            [&channelId, this](Ids::ChannelId const& cid, TerminalChannel& channel) {
                if (cid != *channelId)
                    return true;
                channel.replayContent(impl_->pendingScrollbackReplay.front());
                impl_->pendingScrollbackReplay.pop_front();
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

    // Only lock and snapshot the primary (SSH) channels — local-shell channels
    // are independent of the transport and must keep functioning.
    impl_->frontendSessionManager.value()->connectionLossMode(
        true, FrontendSessionManager::EngineFilter::PrimaryOnly
    );
    impl_->terminalPanel->captureChannelContentsForLockedMode();
    impl_->frontendSessionManager.value()->broadcast(
        language->get("sessionFrontend", "connectionLostTerminalMessage"),
        FrontendSessionManager::EngineFilter::PrimaryOnly
    );

    // Show the per-session reconnect dialog.  Non-modal so local-shell
    // panels (and other tabs) keep working; draggable so the user can
    // move it out of the way.  The dialog's lifetime is bound to the
    // Session — we never explicitly close it since the Session replace
    // tears its DOM down when the reconnect succeeds.
    if (impl_->connectionLostDialog)
    {
        impl_->connectionLostDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "connectionLost"),
            .buttons = ScriptNuiComponents::Dialog::Button::Unknown,
            .modal = false,
            .mayCloseWithoutButton = true,
            .draggable = true,
        });
    }
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

    // Always dispose the FrontendSessionManager first, for BOTH engine types.
    // Skipping this path (the previous behaviour for ExecutingSessionOptions)
    // let widget detach during removePanel start an async channel-dispose chain
    // (via the deleter → onChannelClosedByUser → FSM::closeChannel → engine →
    // ProcessStore::exit RPC) whose completion callback captured `this` through
    // ExecutingTerminalEngine::Implementation — when the RPC response arrived
    // after Session destruction it read freed memory as `impl_->termId`,
    // producing the "Failed to get terminal with id to dispose it: <garbage>"
    // UAF crash. Disposing the FSM first drains every channel's RPC while the
    // Session is still alive; subsequent deleter-triggered closeChannel calls
    // hit the `guardDisposal` short-circuit and do nothing.
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
    SessionSnapshot out;

    // Primary SSH channel scrollback, in FSM iteration order.  Must be read
    // BEFORE the FSM is disposed — getAllTextContent goes through the xterm
    // serializeAddon, which lives inside the terminal DOM element.
    if (impl_->frontendSessionManager.value())
    {
        impl_->frontendSessionManager.value()->forEachChannel(
            FrontendSessionManager::EngineFilter::PrimaryOnly,
            [&out](Ids::ChannelId const&, TerminalChannel& channel) {
                out.primaryChannelScrollback.push_back(channel.getAllTextContent());
                return true;
            }
        );
    }

    // File-explorer side state
    auto captureSide = [](NuiFileExplorer::Side& side) {
        FileExplorerSideSnapshot snap;
        snap.currentPath = side.path();
        snap.flavor = side.flavor();
        snap.showHiddenFiles = side.showHiddenFiles();
        snap.iconSize = side.iconSize();
        snap.iconSpacing = side.iconSpacing();
        snap.sort = side.sort();
        snap.placesOpen = side.placesOpen();
        snap.placesWide = side.placesWide();
        return snap;
    };
    out.local = captureSide(localFileGridSide());
    if (auto* remote = remoteFileGridSide(); remote)
        out.remote = captureSide(*remote);

    out.remoteUsername = impl_->remoteUsername;
    out.sftpWasOpen = impl_->sftpIsOpen;
    out.inFlightOps = impl_->operationQueue.snapshotInFlight();
    Log::info("captureSnapshot: captured {} in-flight operation(s)", out.inFlightOps.size());

    // Local-shell channels: optionally evict them from the aux engine so the
    // adopting Session can pick them up at the backend level.  We skip this
    // when captureSnapshot is used non-destructively (ProtoSession flow —
    // the real eject happens later via ejectLocalShellsForHandoff at the
    // moment of swap, so the preserved old Session keeps working local
    // shells if the user cancels the cycle).
    if (withEjection)
        out.ejectedLocalShells = ejectLocalShellsForHandoff();

    // Lumino layout — reuses the same serialisation getLayout() produces for
    // layout persistence, so the reconnect restore is byte-compatible.
    if (auto layout = getLayout(); layout)
        out.luminoLayout = std::move(*layout);

    return out;
}

std::vector<LocalShellAdoption> Session::ejectLocalShellsForHandoff()
{
    std::vector<LocalShellAdoption> out;
    if (!impl_->frontendSessionManager.value())
        return out;

    // Dump scrollback while the frontend wrappers still exist, THEN eject.
    // Ejection destroys the frontend ExecutingChannel (unregistering its
    // receivers) but leaves the backend process alive, ready to be adopted
    // by the new Session's aux engine.
    std::unordered_map<Ids::ChannelId, std::string, Ids::IdHash> auxScrollback;
    impl_->frontendSessionManager.value()->forEachChannel(
        FrontendSessionManager::EngineFilter::LocalShellOnly,
        [&auxScrollback](Ids::ChannelId const& channelId, TerminalChannel& channel) {
            auxScrollback.emplace(channelId, channel.getAllTextContent());
            return true;
        }
    );

    auto* auxEngine = impl_->frontendSessionManager.value()->auxiliaryLocalShellEngine();
    if (!auxEngine)
        return out;

    auto ejected = auxEngine->ejectAllChannels();
    out.reserve(ejected.size());
    for (auto const& entry : ejected)
    {
        auto const* meta = impl_->terminalPanel->findLocalShellMeta(entry.processId);
        if (!meta)
        {
            Log::warn(
                "Ejected local-shell '{}' has no tracked metadata — dropping",
                entry.processId.value()
            );
            continue;
        }

        std::string scrollback;
        if (auto sbIter = auxScrollback.find(entry.processId); sbIter != auxScrollback.end())
            scrollback = std::move(sbIter->second);

        out.push_back(
            LocalShellAdoption{
                .processId = entry.processId,
                .shellConfigName = meta->shellConfigName,
                .cmdline = meta->cmdline,
                .terminalOptions = meta->terminalOptions,
                .termios = meta->termios,
                .execOpts = meta->execOpts,
                .savedScrollback = std::move(scrollback),
                .stdoutReceptacle = entry.stdoutReceptacle,
                .stderrReceptacle = entry.stderrReceptacle,
            }
        );
    }
    // After ejection the engine's channel map is empty; clear our own
    // tracking to match.  Any remaining entries would be stale.
    impl_->terminalPanel->clearLocalShellMeta();
    return out;
}

void Session::applySnapshot(SessionSnapshot const& snapshot)
{
    // File-explorer side prefs — apply before navigation so the first listing
    // renders with the correct flavor / hidden-file visibility.
    auto applySide = [](NuiFileExplorer::Side& side, FileExplorerSideSnapshot const& snap) {
        side.flavor(snap.flavor);
        side.iconSize(snap.iconSize);
        side.iconSpacing(snap.iconSpacing);
        side.showHiddenFiles(snap.showHiddenFiles);
        side.sort(snap.sort);
        side.placesOpen(snap.placesOpen);
        side.path(snap.currentPath);
    };
    applySide(localFileGridSide(), snapshot.local);
    if (auto* remote = remoteFileGridSide(); remote)
        applySide(*remote, snapshot.remote);

    // Queue scrollback for replay into primary channels as they open.  On
    // the reconnect path applySnapshot fires before initializeLayout has
    // built the channels (and even after that, each xterm only gets a live
    // termId inside its own onMaterialize), so iterating frontend channels
    // here would find none.  onOpenChannel drains the front entry per
    // successfully-opened primary channel.  Captured order matches the FSM
    // iteration order used in captureSnapshot.
    impl_->pendingScrollbackReplay.clear();
    for (auto const& dump : snapshot.primaryChannelScrollback)
        impl_->pendingScrollbackReplay.push_back(dump);

    // Re-enqueue interrupted operations.  Single-file kinds go through
    // OperationQueue::enqueueResumable directly; bulk kinds are adopted
    // server-side via SessionManager::adoptBulkResume so the backend's
    // saved entry list never crosses the IPC boundary.  Their OperationIds
    // are also remembered for destructor-time cleanup.
    Log::info(
        "applySnapshot: replaying {} in-flight operation(s) from snapshot",
        snapshot.inFlightOps.size()
    );
    std::vector<std::string> bulkResumeIds;
    for (auto const& op : snapshot.inFlightOps)
    {
        switch (op.kind)
        {
            case ResumableOp::Kind::Download:
            case ResumableOp::Kind::Upload:
            case ResumableOp::Kind::Delete:
            case ResumableOp::Kind::Rename:
                impl_->operationQueue.enqueueResumable(op);
                break;
            case ResumableOp::Kind::BulkDownload:
            case ResumableOp::Kind::BulkUpload:
            case ResumableOp::Kind::BulkDelete:
                if (op.operationId.isValid())
                {
                    bulkResumeIds.push_back(op.operationId.value());
                    impl_->trackedBulkResumes.push_back(op.operationId);
                }
                break;
        }
    }
    if (!bulkResumeIds.empty() && impl_->frontendSessionManager.value() &&
        impl_->frontendSessionManager.value()->engine().engineName() == "ssh")
    {
        auto* sshTerminalEngine =
            static_cast<SshTerminalEngine*>(&impl_->frontendSessionManager.value()->engine());
        Nui::val args = Nui::val::object();
        args.set("sessionId", sshTerminalEngine->sshSessionId().value());
        Nui::val opIds = Nui::val::array();
        for (std::size_t idx = 0; idx < bulkResumeIds.size(); ++idx)
            opIds.set(idx, bulkResumeIds[idx]);
        args.set("operationIds", opIds);
        Nui::RpcClient::callWithBackChannel(
            "SessionManager::adoptBulkResume",
            [count = bulkResumeIds.size()](Nui::val val) {
                if (val.hasOwnProperty("error"))
                {
                    Log::error(
                        "adoptBulkResume failed for {} bulk(s): {}",
                        count,
                        val["error"].as<std::string>()
                    );
                }
                else
                {
                    Log::info("adoptBulkResume queued {} bulk operation(s)", count);
                }
            },
            args
        );
    }

    // pendingLocalShellAdoptions is seeded in the Implementation constructor
    // from pendingResumeSnapshot — by the time applySnapshot runs, Lumino
    // layout restore may already have consumed entries from it, so we must
    // not overwrite what's left with the raw snapshot list.
}

void Session::reconnect()
{
    if (!impl_->isInLostConnectionState.value())
    {
        Log::warn("Session::reconnect called outside the lost-connection state — ignoring");
        return;
    }
    if (impl_->reconnectCycleActive.value())
    {
        Log::warn("Session::reconnect called while a reconnect cycle is already active — ignoring");
        return;
    }
    if (!impl_->requestReconnect)
    {
        Log::error("Session::reconnect: no requestReconnect callback wired");
        return;
    }

    Log::info("Session::reconnect: capturing snapshot and handing off to SessionArea");
    // Non-destructive capture: the old Session stays alive (user sees its
    // lost-connection overlay with the reconnect cycle UI on top) while a
    // ProtoSession probes the transport.  SessionArea calls back to
    // ejectLocalShellsForHandoff on this Session only at the moment of
    // swap, so cancelling mid-cycle leaves local shells working.
    auto snapshot = captureSnapshot(/*withEjection=*/false);
    impl_->requestReconnect(this, std::move(snapshot));
}

void Session::startReconnectUi()
{
    impl_->reconnectCycleActive = true;
    impl_->reconnectAttempt = 1;
    impl_->reconnectCountdown = 0;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void Session::stopReconnectUi()
{
    impl_->reconnectCycleActive = false;
    impl_->reconnectAttempt = 1;
    impl_->reconnectCountdown = 0;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void Session::setReconnectUiAttempt(int attempt)
{
    impl_->reconnectAttempt = attempt;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

void Session::setReconnectUiCountdown(int seconds)
{
    impl_->reconnectCountdown = seconds;
    Nui::globalEventContext.executeActiveEventsImmediately();
}

std::optional<SessionSnapshot> Session::takePendingSnapshot()
{
    if (!impl_->pendingResumeSnapshot)
        return std::nullopt;
    auto out = std::move(*impl_->pendingResumeSnapshot);
    impl_->pendingResumeSnapshot.reset();
    return out;
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

auto Session::makeOperationQueueElement() -> Nui::ElementRenderer
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    namespace Snc = ScriptNuiComponents;

    // clang-format off
    return div{
        style = "width: 100%; height: 100%; position: relative; display: block",
    }(
        impl_->operationQueue(),
        div{
            style = observe(impl_->isInLostConnectionState).generate([this](){
                return fmt::format("display: {};", impl_->isInLostConnectionState.value() ? "block" : "none");
            }),
            class_ = "session-panel-blocker",
        }()
    );
    // clang-format on
}

auto Session::makeFileTrackingElement() -> Nui::ElementRenderer
{
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        style = "width: 100%; height: 100%; position: relative; display: block",
    }(
        impl_->fileTrackingPanel(),
        div{
            style = observe(impl_->isInLostConnectionState).generate([this](){
                return fmt::format("display: {};", impl_->isInLostConnectionState.value() ? "block" : "none");
            }),
            class_ = "session-panel-blocker",
        }()
    );
    // clang-format on
}

void Session::onChannelClosedByUser(Ids::ChannelId const& channelId)
{
    impl_->terminalPanel->onChannelClosedByUser(channelId);
}

void Session::loadLayoutExtras(nlohmann::json const& layoutExtra)
{
    if (layoutExtra.contains("fileGrid"))
    {
        auto fileGridExtra = layoutExtra["fileGrid"];
        if (fileGridExtra.contains("leftSide") && fileGridExtra["leftSide"].contains("flavor"))
        {
            impl_->fileExplorerPanel.localFileGridSide().flavor(
                NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["leftSide"]["flavor"].get<std::string>())
            );
        }
        if (fileGridExtra.contains("rightSide") && fileGridExtra["rightSide"].contains("flavor") &&
            impl_->fileExplorerPanel.remoteFileGridSide())
        {
            impl_->fileExplorerPanel.remoteFileGridSide()->flavor(
                NuiFileExplorer::fileGridFlavorFromString(fileGridExtra["rightSide"]["flavor"].get<std::string>())
            );
        }
    }
}

void Session::initializeLayout()
{
    Log::info("Trying to initialize session layout...");

    Nui::val element;
    if (auto host = impl_->layoutHost.lock(); host)
    {
        Log::info("Layout host found, initializing layout");
        element = host->val();
    }
    else
    {
        Log::info("Waiting for layout host");
        impl_->waitingForLayoutHost = true;
        return;
    }

    std::optional<nlohmann::json> layout = std::nullopt;

    // Reconnect path: prefer the snapshot's layout over the named saved one.
    // This round-trips the exact set of panels (terminals, file explorer,
    // local-shell tabs) the user had open at the moment of disconnection.
    // Adoption routes through pendingResumeLayout because its onOpenSession
    // resets the snapshot before the DOM-triggered initializeLayout runs;
    // fresh-reconnect still has the snapshot around the first time through.
    if (!impl_->pendingResumeLayout.is_null())
    {
        Log::info("Initializing layout from pending resume layout");
        layout = impl_->pendingResumeLayout;
        impl_->pendingResumeLayout = nlohmann::json{};
    }
    else if (impl_->pendingResumeSnapshot && !impl_->pendingResumeSnapshot->luminoLayout.is_null())
    {
        Log::info("Initializing layout from resume snapshot");
        layout = impl_->pendingResumeSnapshot->luminoLayout;
    }
    else if (impl_->layoutName != Constants::defaultLayoutName)
    {
        if (impl_->engineOptions.layouts && impl_->layoutName)
        {
            if (auto iter = impl_->engineOptions.layouts->find(*impl_->layoutName);
                iter != impl_->engineOptions.layouts->end())
            {
                layout = iter->second;
            }
            else
            {
                Log::warn("Layout name not found: {}", *impl_->layoutName);
            }
        }
    }

    Log::info(
        "Initializing layout with name '{}': {}",
        impl_->layoutName.value_or("(none)"),
        layout ? layout->dump() : "(none)"
    );

    if (layout && layout->contains("__extra"))
        loadLayoutExtras((*layout)["__extra"]);

    auto addPanelArgument = Nui::val::object();
    addPanelArgument.set("host", element);
    addPanelArgument.set("id", impl_->sessionLayoutId);
    addPanelArgument.set("engineType", impl_->frontendSessionManager.value()->engine().engineName());
    addPanelArgument.set("layoutString", layout ? Nui::val(layout->dump()) : Nui::val::undefined());
    addPanelArgument.set(
        "terminalFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                Nui::WebApi::Console::log("Channel factory content panel manager");
                auto elem = Nui::Dom::makeStandaloneElement(makeChannelElement());
                impl_->terminalPanel->channelElements().push_back(elem);
                return elem->val();
            }
        )
    );
    addPanelArgument.set(
        "terminalDelete",
        Nui::bind(
            [this](Nui::val channelIdVal) -> Nui::val
            {
                Nui::WebApi::Console::log(channelIdVal);

                if (channelIdVal.isUndefined())
                {
                    Log::critical("Channel id is undefined");
                    return Nui::val::undefined();
                }

                if (channelIdVal.isString())
                {
                    Ids::ChannelId channelId = Ids::makeChannelId(channelIdVal.as<std::string>());
                    if (!channelId.isValid())
                    {
                        Log::critical("Channel id is not valid");
                        return Nui::val::undefined();
                    }

                    onChannelClosedByUser(channelId);
                }
                else
                {
                    Log::critical("Channel id is not a string");
                }
                return Nui::val::undefined();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "fileExplorerFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                // OpenFileExplorer
                auto& fileExplorerElement = impl_->fileExplorerPanel.elementObservable();
                if (fileExplorerElement.value())
                {
                    Log::warn("There is already a file explorer, cannot open another one");
                    return Nui::val::undefined();
                }
                fileExplorerElement = Nui::Dom::makeStandaloneElement(makeFileExplorerElement());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return fileExplorerElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "fileExplorerDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                // Remove FileExplorer
                auto& fileExplorerElement = impl_->fileExplorerPanel.elementObservable();
                if (!fileExplorerElement.value())
                {
                    Log::warn("There is no file explorer to remove");
                    return Nui::val::undefined();
                }
                Nui::WebApi::Console::log("Removing file explorer element");
                fileExplorerElement.value().reset();
                fileExplorerElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "operationQueueFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->operationQueueElement.value())
                {
                    Log::warn("There is already an operation queue, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->operationQueueElement = Nui::Dom::makeStandaloneElement(makeOperationQueueElement());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->operationQueueElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "operationQueueDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->operationQueueElement.value())
                {
                    Log::warn("There is no operation queue to remove");
                    return Nui::val::undefined();
                }
                impl_->operationQueueElement.value().reset();
                impl_->operationQueueElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "sessionOptionsFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->sessionOptionsElement.value())
                {
                    Log::warn("There are already session options, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->sessionOptionsElement = Nui::Dom::makeStandaloneElement(impl_->sessionOptions());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->sessionOptionsElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "sessionOptionsDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->sessionOptionsElement.value())
                {
                    Log::warn("There are no session options to remove");
                    return Nui::val::undefined();
                }
                impl_->sessionOptionsElement.value().reset();
                impl_->sessionOptionsElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "fileTrackingFactory",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (impl_->fileTrackingElement.value())
                {
                    Log::warn("There is already a file tracking panel, cannot open another one");
                    return Nui::val::undefined();
                }
                impl_->fileTrackingElement = Nui::Dom::makeStandaloneElement(makeFileTrackingElement());
                Nui::globalEventContext.executeActiveEventsImmediately();
                return impl_->fileTrackingElement.value()->val();
            }
        )
    );
    addPanelArgument.set(
        "fileTrackingDelete",
        Nui::bind(
            [this]() -> Nui::val
            {
                if (!impl_->fileTrackingElement.value())
                {
                    Log::warn("There is no file tracking panel to remove");
                    return Nui::val::undefined();
                }
                impl_->fileTrackingElement.value().reset();
                impl_->fileTrackingElement.modifyNow();
                return Nui::val::undefined();
            }
        )
    );
    addPanelArgument.set(
        "localShellFactory",
        Nui::bind(
            [this](Nui::val shellNameVal) -> Nui::val
            {
                if (!shellNameVal.isString())
                {
                    Log::error("localShellFactory called without shell name string");
                    return Nui::val::undefined();
                }
                const std::string shellName = shellNameVal.as<std::string>();

                // Reconnect path: if there's a pending adoption for this shell
                // name, adopt it rather than spawning a fresh process.  Ordered
                // consumption — the first adoption with a matching shellConfigName
                // wins — so multi-tab restoration maps adoptions to tabs in the
                // order Lumino materializes them (which matches the order they
                // were captured).
                if (!impl_->pendingLocalShellAdoptions.empty())
                {
                    auto match = std::ranges::find_if(
                        impl_->pendingLocalShellAdoptions,
                        [&shellName](LocalShellAdoption const& adoption) {
                            return adoption.shellConfigName == shellName;
                        }
                    );
                    if (match != impl_->pendingLocalShellAdoptions.end())
                    {
                        Log::info("localShellFactory: adopting local shell '{}'", shellName);
                        LocalShellAdoption adoption = std::move(*match);
                        impl_->pendingLocalShellAdoptions.erase(match);
                        auto elem = Nui::Dom::makeStandaloneElement(
                            makeAdoptedLocalShellChannelElement(std::move(adoption))
                        );
                        impl_->terminalPanel->channelElements().push_back(elem);
                        return elem->val();
                    }
                }

                // Probe state before building the element. A saved layout may
                // reference a shell config the user has since removed — in that
                // case return undefined so the TS side drops the widget rather
                // than showing a dead/blank tab.
                auto const& sessions = impl_->stateHolder->stateCache().sessions;
                auto iter = sessions.find(shellName);
                if (iter == sessions.end() ||
                    !std::holds_alternative<Persistence::ExecutingSessionOptions>(iter->second.engine))
                {
                    Log::warn("localShellFactory: shell '{}' no longer in settings — dropping widget", shellName);
                    return Nui::val::undefined();
                }

                Log::info("localShellFactory: spawning local shell '{}'", shellName);
                auto elem = Nui::Dom::makeStandaloneElement(makeLocalShellChannelElement(shellName));
                impl_->terminalPanel->channelElements().push_back(elem);
                return elem->val();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "localShellDelete",
        Nui::bind(
            [this](Nui::val channelIdVal) -> Nui::val
            {
                if (channelIdVal.isUndefined())
                {
                    Log::warn("localShellDelete: channel id undefined (never spawned)");
                    return Nui::val::undefined();
                }
                if (!channelIdVal.isString())
                {
                    Log::error("localShellDelete: channel id is not a string");
                    return Nui::val::undefined();
                }
                Ids::ChannelId channelId = Ids::makeChannelId(channelIdVal.as<std::string>());
                if (!channelId.isValid())
                {
                    Log::error("localShellDelete: channel id is invalid");
                    return Nui::val::undefined();
                }
                onChannelClosedByUser(channelId);
                return Nui::val::undefined();
            },
            std::placeholders::_1
        )
    );
    addPanelArgument.set(
        "openAddContextMenu",
        Nui::bind(
            [this](Nui::val val)
            {
                Log::info("openAddContextMenu called");
                if (!val.isString())
                {
                    Log::error("openAddContextMenu needs to be called with a string argument");
                }
                // Refresh entries so the Local Shell list reflects current saved
                // shell sessions and single-instance panel state is accurate.
                impl_->rebuildTabAddMenu();
                Nui::globalEventContext.executeActiveEventsImmediately();
                impl_->tabAddMenu.openNextTo(val.as<std::string>());
            },
            std::placeholders::_1
        )
    );

    Log::info("Adding panel to content panel manager with layout id '{}'", impl_->sessionLayoutId);
    const auto addPanelResult = Nui::val::global("contentPanelManager").call<bool>("addPanel", addPanelArgument);
    if (!addPanelResult)
    {
        Log::error("Failed to add panel to content panel manager");
        impl_->confirmDialog->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sessionFrontend", "layoutCreationFailedHeader"),
            .text = language->get("sessionFrontend", "layoutCreationFailedText"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "layoutCreationFailed",
        });
        closeSelf();
    }
    Log::info("Panel added to content panel manager successfully");
}

Nui::ElementRenderer Session::operator()()
{
    using Nui::Elements::div; // because of the global div.
    using Nui::Elements::span;
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
            impl_->layoutHost = elem.lock();
            try {
                if (impl_->waitingForLayoutHost) {
                    impl_->waitingForLayoutHost = false;
                    Nui::globalEventContext.delayToAfterProcessing([this](){
                        Log::info("Layout host attached to DOM, initializing layout");
                        initializeLayout();
                    });
                }
            } catch (const std::exception& e) {
                Log::error("Error while initializing layout in layout host: {}", e.what());
            }
        }),
        "inert"_attr = observe(impl_->inertEverything).generate([this]() -> std::optional<std::string> {
            return impl_->inertEverything.value() ? "true"s : std::optional<std::string>{std::nullopt};
        })
    }(
        impl_->tabAddMenu(),
        impl_->syncDialog(),
        impl_->syncProgressDialog(),
        // Per-session reconnect dialog — non-modal and draggable.  Opened
        // in onConnectionLoss; torn down with the Session on swap.  The
        // view blockers themselves live inside the individual SFTP-bound
        // panel renderers (file explorer, operation queue, file tracking)
        // so terminal panels stay fully usable during a reconnect cycle.
        (*impl_->connectionLostDialog)()
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