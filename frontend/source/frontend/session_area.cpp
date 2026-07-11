#include <frontend/session_area.hpp>
#include <frontend/session.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/proto_session.hpp>
#include <log/log.hpp>
#include <events/app_event_context.hpp>

#include <script-nui-components/tabs.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>
#include <nui/rpc.hpp>

#include <list>
#include <unordered_map>
#include <variant>

/**
 * @brief Per-tab retry state for an in-progress reconnect cycle.  Owned by
 *        SessionArea.  The old (visible) Session stays rendered in
 *        impl_->sessions untouched for the duration of the cycle; only the
 *        headless ProtoSession candidate lives here, along with the retry
 *        metadata.
 */
struct ReconnectState
{
    SessionSnapshot snapshot;
    int attempt{1};
    /// Session name (for the loadState fetch path on each retry).
    std::string name;
    /// Snapshot-free engine options — needed to construct each retry proto.
    Persistence::SessionOptions engineOptions;
    Persistence::UiOptions uiOptions;
    /// Cached layoutName so the adopting Session keeps the right saved-layout
    /// id if its snapshot lumino layout is ever cleared.
    std::optional<std::string> layoutName;
    /// setTimeout handle for the pending backoff timer; cleared on cancel /
    /// on retry firing.  Holds Nui::val::undefined() when no retry is pending.
    Nui::val retryTimerHandle{Nui::val::undefined()};
    /// setTimeout handle for the per-second countdown tick.
    Nui::val countdownTimerHandle{Nui::val::undefined()};
    /// In-flight probe.  Replaced on every new attempt; reset to null while
    /// the backoff timer is running.  Never held here while the old Session
    /// is trying to reconnect itself — only SessionArea keeps the proto.
    std::unique_ptr<ProtoSession> candidate;
};

struct SessionArea::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* newItemAskDialog;
    ConfirmDialog* confirmDialog;
    FilePropertyDialog* filePropertyDialog;
    ArchiveTransferDialog* archiveTransferDialog;
    Toolbar* toolbar;
    CommandStoreClient* commandStoreClient;
    Nui::Observed<std::vector<std::unique_ptr<Session>>> sessions;
    Nui::RpcClient::AutoUnregister dropHandlerUnregister_;
    ScriptNuiComponents::Tabs tabs;
    Nui::ListenRemover<decltype(FrontendEvents::onNewSession)> newSessionListener{};

    /// Active reconnect cycles, keyed by outer tabId.  Each cycle preserves
    /// its old Session in impl_->sessions untouched and drives a headless
    /// ProtoSession probe in the background until either the probe succeeds
    /// (swap in) or the user cancels.
    std::unordered_map<int, ReconnectState> reconnectStates;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* newItemAskDialog,
        ConfirmDialog* confirmDialog,
        FilePropertyDialog* filePropertyDialog,
        ArchiveTransferDialog* archiveTransferDialog,
        Toolbar* toolbar,
        CommandStoreClient* commandStoreClient
    )
        : stateHolder{stateHolder}
        , events{events}
        , newItemAskDialog{newItemAskDialog}
        , confirmDialog{confirmDialog}
        , filePropertyDialog{filePropertyDialog}
        , archiveTransferDialog{archiveTransferDialog}
        , toolbar{toolbar}
        , commandStoreClient{commandStoreClient}
        , sessions{}
        , dropHandlerUnregister_{}
        , tabs{}
        , reconnectStates{}
    {}
};

void SessionArea::removeActiveSession()
{
    removeSession(impl_->tabs.selectedId());
}

SessionArea::SessionArea(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* newItemAskDialog,
    ConfirmDialog* confirmDialog,
    FilePropertyDialog* filePropertyDialog,
    ArchiveTransferDialog* archiveTransferDialog,
    Toolbar* toolbar,
    CommandStoreClient* commandStoreClient
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          newItemAskDialog,
          confirmDialog,
          filePropertyDialog,
          archiveTransferDialog,
          toolbar,
          commandStoreClient
      )}
{
    impl_->tabs.onClose(
        [this](int id)
        {
            // Confirm Dialog?
            removeSession(id);
            return false;
        }
    );

    impl_->tabs.onSelect(
        [this](int tabId) -> bool
        {
            Nui::WebApi::Console::log("Tab with id '{}' selected.", tabId);
            setSelected(tabId);
            return false;
        }
    );

    impl_->newSessionListener = Nui::smartListen(
        events->onNewSession,
        [this](std::string const& name) -> void
        {
            Log::info("onNewSession Event: Adding session with name '{}'", name);
            addSession(name);
        }
    );

    loadState(
        *stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const& state)
        {
            if (!success)
                return;

            for (auto const& [name, session] : state.sessions)
            {
                if (session.startupSession)
                    addSession(name);
            }
        }
    );

    registerRpc();
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(SessionArea);

void SessionArea::registerRpc()
{
    // TODO:
    // Nui::RpcClient::registerFunction(
    //     "SessionArea::processDied",
    //     [this](Nui::val val)
    //     {
    //         auto const processId = val["id"].as<std::string>();
    //         Log::info("Process with id '{}' terminated.", processId);

    //         std::size_t index = 0;
    //         int tabId = -1;
    //         for (auto const& session : impl_->sessions.value())
    //         {
    //             if (session->getProcessIdIfExecutingEngine().value_or("") == processId)
    //             {
    //                 Log::info("Process with id '{}' found in session '{}'.", processId, session->name());
    //                 tabId = session->tabId();
    //                 break;
    //             }
    //             ++index;
    //         }

    //         if (index < impl_->sessions.value().size())
    //             removeSession(tabId);
    //         else
    //         {
    //             Log::error("Process with id '{}' not found in any session.", processId);
    //         }
    //     }
    // );

    impl_->dropHandlerUnregister_ = Nui::RpcClient::autoRegisterFunction(
        "SessionArea::onFilesDropped",
        [this](Nui::val val)
        {
            const auto json = nlohmann::json::parse(Nui::JSON::stringify(val));
            const auto dropMetadata = json.value("dropMetadata", "");
            const auto isLeft = json.value("isLeft", false);

            Session* session = getSessionByLayoutId(dropMetadata);
            if (!session)
            {
                Log::error("No session found for dropMetadata '{}'", dropMetadata);
                return;
            }

            std::optional<std::string> subdir{std::nullopt};
            if (json.contains("subdir"))
                subdir = json["subdir"].get<std::string>();

            session->onDrop(
                isLeft,
                json.value("entries", nlohmann::json::array()).get<std::vector<SharedData::DirectoryEntry>>(),
                subdir
            );
        }
    );
}

void SessionArea::removeSession(int tabId)
{
    const auto indexOpt = findSessionIndexByTabId(tabId);
    if (!indexOpt)
    {
        Log::error("Session index out of bounds: {}", tabId);
        return;
    }
    const auto index = *indexOpt;

    Log::info("Removing session: {}", impl_->sessions.value()[index]->name());

    if (impl_->sessions.value()[index]->visible() && impl_->sessions.value().size() > 1)
        setSelected(impl_->tabs.firstTabId());

    impl_->sessions.value()[index]->shutdown(
        [this, index, tabId]()
        {
            impl_->sessions.erase(impl_->sessions.begin() + index);
            impl_->tabs.remove(tabId);
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void SessionArea::setSelected(int tabId)
{
    const int previousSelected = impl_->tabs.selectedId();
    auto sync = Nui::ScopeExit{[]() noexcept
        {
            Nui::globalEventContext.sync();
        }};

    if (previousSelected != -1)
    {
        const auto previousSessionIndex = findSessionIndexByTabId(previousSelected);
        if (previousSessionIndex)
            impl_->sessions.value()[*previousSessionIndex]->visible(false);
    }
    impl_->tabs.select(tabId);
    const auto newIndex = findSessionIndexByTabId(tabId);
    if (!newIndex)
    {
        Log::error("Tab with id '{}' not found for selection.", tabId);
        return;
    }
    Log::info("Selected session: {}", *newIndex);
    impl_->sessions.value()[*newIndex]->visible(true);
}

std::optional<std::size_t> SessionArea::findSessionIndexByTabId(int tabId) const
{
    for (size_t i = 0; i != impl_->sessions.value().size(); ++i)
    {
        if (impl_->sessions.value()[i]->tabId() == tabId)
            return i;
    }
    return std::nullopt;
}

Session::Params SessionArea::makeSessionParams(
    Persistence::SessionOptions engineOptions,
    Persistence::UiOptions uiOptions,
    std::string displayName,
    std::optional<std::string> layoutName,
    bool visible,
    int tabId,
    std::optional<SessionSnapshot> resumeFromSnapshot
)
{
    using namespace std::string_literals;

    Session::Params params;
    params.stateHolder = impl_->stateHolder;
    params.events = impl_->events;
    params.newItemAskDialog = impl_->newItemAskDialog;
    params.confirmDialog = impl_->confirmDialog;
    params.filePropertyDialog = impl_->filePropertyDialog;
    params.archiveTransferDialog = impl_->archiveTransferDialog;
    params.commandStoreClient = impl_->commandStoreClient;
    params.sessionOptions = std::move(engineOptions);
    params.uiOptions = std::move(uiOptions);
    params.initialName = std::move(displayName);
    params.layoutName = std::move(layoutName);
    params.visible = visible;
    params.tabId = tabId;
    params.resumeFromSnapshot = std::move(resumeFromSnapshot);

    params.closeSelf = [this](Session const* ptr) {
        removeSession(ptr->tabId());
    };
    params.disambiguateTitle = [this](Session const* ptr, std::string const& desiredTitle) -> std::string {
        std::string disambiguatedTitle = desiredTitle;
        int suffix = 1;
        bool titleExists = false;

        do
        {
            titleExists = false;
            for (auto const& session : impl_->sessions.value())
            {
                if (auto tabTitle = session->tabTitle().lock();
                    tabTitle && tabTitle->value() == disambiguatedTitle)
                {
                    titleExists = true;
                    disambiguatedTitle = desiredTitle + " ("s + std::to_string(suffix) + ")"s;
                    ++suffix;
                    break;
                }
            }
        } while (titleExists);

        impl_->tabs.modifyTabById(
            ptr->tabId(),
            [&disambiguatedTitle](ScriptNuiComponents::Tabs::Tab* tab) {
                if (!tab)
                {
                    Log::error("Tab not found for modifying title.");
                    return;
                }
                tab->title = disambiguatedTitle;
            }
        );
        return disambiguatedTitle;
    };

    params.requestReconnect = [this](Session const* ptr, SessionSnapshot snapshot) {
        replaceAtTabId(ptr->tabId(), std::move(snapshot), /*attempt=*/1);
    };
    // Adoption succeeded — no SessionArea-level wrap-up left here (the swap
    // in replaceAtTabId already cleared the retry state).  Kept as a hook
    // for any future telemetry.
    params.onReconnectSucceeded = [](Session const* /*ptr*/) {};
    // ProtoSession carries the actual "open failed" signal now, so the old
    // Session-driven failure callback is unused.  Left empty rather than
    // removed from Params to keep the wiring surface stable for the local-
    // shell / executing path that still uses the fresh Session ctor.
    params.onReconnectFailed = [](Session const* /*ptr*/) {};
    params.onReconnectCancel = [this](Session const* ptr) {
        abortReconnectCycle(ptr->tabId());
    };
    params.onReconnectNow = [this](Session const* ptr) {
        fireReconnectRetryNow(ptr->tabId());
    };

    return params;
}

void SessionArea::addSession(std::string const& name)
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, name](bool success, Persistence::State const& state)
        {
            if (!success)
            {
                Log::error("Failed to load state while adding session '{}'", name);
                return;
            }

            auto iter = state.sessions.find(name);
            if (iter == end(state.sessions))
            {
                Log::error("No engine found for name: {}", name);
                return;
            }

            auto [engineKey, engine] = *iter;

            const auto tabId = impl_->tabs.add(name, true);
            Log::info("Adding session: {} with layout {}. Tab ID: {}", name, impl_->toolbar->selectedLayout(), tabId);

            impl_->sessions.emplace_back(std::make_unique<Session>(makeSessionParams(
                std::move(engine),
                state.uiOptions,
                name,
                impl_->toolbar->selectedLayout(),
                impl_->sessions.size() == 0,
                tabId,
                std::nullopt
            )));

            setSelected(tabId);
        },
        "Cannot add session."
    );
}

void SessionArea::addDirectConnectSession(Persistence::SshSessionOptions const& sshOptions)
{
    using namespace std::string_literals;

    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, sshOptions](bool success, Persistence::State const& state)
        {
            if (!success)
            {
                Log::error("Failed to load state while adding direct connect session");
                return;
            }

            Persistence::SessionOptions engine =
                Persistence::SessionOptions::create(std::nullopt, Persistence::TerminalEngineType::ssh);
            auto& engineSsh = std::get<Persistence::SshSessionOptions>(engine.engine);
            engineSsh.host = sshOptions.host;
            engineSsh.port = sshOptions.port;
            engineSsh.user = sshOptions.user;
            engineSsh.sshKeyPrivate = sshOptions.sshKeyPrivate;
            engineSsh.sshKeyPublic = sshOptions.sshKeyPublic;
            engineSsh.openSftpByDefault = sshOptions.openSftpByDefault;
            engineSsh.remoteFavorites = sshOptions.remoteFavorites;

            auto fillDefaults = [](auto& target, auto const& source)
            {
                if (!target.hasReference())
                    return;
                auto iter = source.find(target.ref());
                if (iter != source.end())
                    Persistence::useDefaultsFrom(target, iter->second);
            };
            fillDefaults(engine.terminalOptions, state.terminalOptions);
            fillDefaults(engine.termios, state.termios);
            fillDefaults(engine.queueOptions, state.queueOptions);
            fillDefaults(engineSsh.sshOptions, state.sshOptions);
            fillDefaults(engineSsh.sftpOptions, state.sftpOptions);

            std::string displayName = engineSsh.host;
            if (engineSsh.user)
                displayName = *engineSsh.user + "@" + displayName;
            if (engineSsh.port)
                displayName += ":" + std::to_string(*engineSsh.port);

            const auto tabId = impl_->tabs.add(displayName, true);
            Log::info(
                "Adding direct connect session: {} with layout {}. Tab ID: {}",
                displayName,
                impl_->toolbar->selectedLayout(),
                tabId
            );

            impl_->sessions.emplace_back(std::make_unique<Session>(makeSessionParams(
                std::move(engine),
                state.uiOptions,
                displayName,
                impl_->toolbar->selectedLayout(),
                impl_->sessions.size() == 0,
                tabId,
                std::nullopt
            )));

            setSelected(tabId);
        },
        "Cannot add direct connect session."
    );
}

void SessionArea::replaceAtTabId(int tabId, SessionSnapshot snapshot, int attempt)
{
    Log::info("SessionArea::replaceAtTabId: tabId={}, attempt={}", tabId, attempt);

    const auto sessionIdx = findSessionIndexByTabId(tabId);
    if (!sessionIdx)
    {
        Log::error("replaceAtTabId: tabId {} not found", tabId);
        return;
    }

    auto* oldSession = impl_->sessions.value()[*sessionIdx].get();
    const std::string name = oldSession->name();

    // First-attempt bookkeeping: stash the snapshot / per-tab retry state
    // and flip the old Session's overlay into its in-progress UI.  The old
    // Session stays rendered — only the ProtoSession probe is headless, and
    // the user sees the retry attempt counter + countdown + [Now] + [Cancel]
    // sitting on top of the lost-connection overlay they already had.
    auto [stateIter, inserted] = impl_->reconnectStates.try_emplace(tabId);
    auto& reconnectState = stateIter->second;
    reconnectState.attempt = attempt;
    if (inserted)
    {
        reconnectState.name = name;
        reconnectState.layoutName = impl_->toolbar->selectedLayout();
        reconnectState.snapshot = std::move(snapshot);
        oldSession->startReconnectUi();
    }
    else
    {
        // Later attempt — caller's snapshot is redundant with the stored one.
        (void)snapshot;
    }
    oldSession->setReconnectUiAttempt(attempt);
    oldSession->setReconnectUiCountdown(0);

    // Resolve the *current* engine + ui options from the state cache each
    // attempt so recent settings edits take effect (same "new tabs pick up
    // the latest settings" semantics the user asked for earlier).
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, tabId](bool success, Persistence::State const& state) {
            if (!success)
            {
                Log::error("replaceAtTabId: state load failed");
                abortReconnectCycle(tabId);
                return;
            }

            auto rcIter = impl_->reconnectStates.find(tabId);
            if (rcIter == impl_->reconnectStates.end())
                return; // Cancelled between loadState call and callback.
            auto& reconnectState = rcIter->second;

            auto sessionIter = state.sessions.find(reconnectState.name);
            if (sessionIter == state.sessions.end())
            {
                Log::error(
                    "replaceAtTabId: session '{}' no longer exists in settings — aborting reconnect",
                    reconnectState.name
                );
                abortReconnectCycle(tabId);
                return;
            }
            reconnectState.engineOptions = sessionIter->second;
            reconnectState.uiOptions = state.uiOptions;

            // Build the ProtoSession, hook it up, start it.  The proto
            // carries a COPY of the snapshot so the stored one remains
            // available for subsequent retries; only onProtoReady's adopt
            // path drains the stored snapshot (via takeResumeSnapshot on
            // the proto, which got that copy).
            ProtoSession::Params protoParams;
            protoParams.sessionOptions = reconnectState.engineOptions;
            protoParams.uiOptions = reconnectState.uiOptions;
            protoParams.resumeFromSnapshot = reconnectState.snapshot;
            protoParams.onReady = [this, tabId](ProtoSession*) {
                onProtoReady(tabId);
            };
            protoParams.onFailed = [this, tabId](ProtoSession*, std::string const& info) {
                onProtoFailed(tabId, info);
            };
            reconnectState.candidate = std::make_unique<ProtoSession>(std::move(protoParams));
            reconnectState.candidate->start();
        },
        "Cannot reconnect session."
    );
}

void SessionArea::onProtoReady(int tabId)
{
    Log::info("SessionArea::onProtoReady: tabId={}", tabId);

    auto rcIter = impl_->reconnectStates.find(tabId);
    if (rcIter == impl_->reconnectStates.end())
    {
        Log::warn("onProtoReady: no reconnect state for tabId {} — dropping proto", tabId);
        return;
    }
    auto& reconnectState = rcIter->second;
    auto proto = std::move(reconnectState.candidate);
    if (!proto)
    {
        Log::error("onProtoReady: candidate slot empty for tabId {}", tabId);
        return;
    }

    cancelReconnectTimers(tabId);

    const auto sessionIdx = findSessionIndexByTabId(tabId);
    if (!sessionIdx)
    {
        Log::error("onProtoReady: tabId {} vanished mid-swap", tabId);
        impl_->reconnectStates.erase(tabId);
        return;
    }

    const bool wasVisible = (impl_->tabs.selectedId() == tabId);

    // Evict local-shell channels from the OLD Session's aux engine right
    // before swap — the processes survive as backend-only state for the
    // new Session to adopt, while the old Session's frontend wrappers are
    // cleared in the same breath.  This is the destructive step we
    // deliberately kept out of Session::reconnect so that cancelling a
    // cycle leaves local shells untouched.
    reconnectState.snapshot.ejectedLocalShells =
        impl_->sessions.value()[*sessionIdx]->ejectLocalShellsForHandoff();

    // Hold a shared_ptr to the dying Session so its async shutdown drain
    // can outlive the local scope.  Remove it from the vector via the
    // Observed's erase (NOT value().erase) so Nui sees the modification
    // and unmounts the old Session's DOM — otherwise the lost-connection
    // overlay renders on top of the new Session and looks "stuck".
    auto draining =
        std::shared_ptr<Session>(std::move(impl_->sessions.value()[*sessionIdx]));
    impl_->sessions.erase(impl_->sessions.begin() + *sessionIdx);

    const auto name = reconnectState.name;
    const auto layoutName = reconnectState.layoutName;
    // Move the enriched snapshot into a Params field so the new Session
    // sees the ejected shells via its adoption path.  The adoption ctor's
    // code path honours Params.resumeFromSnapshot when non-empty.
    auto enrichedSnapshot = std::move(reconnectState.snapshot);

    // Drop the retry state now so any stray timer callbacks bail out early.
    impl_->reconnectStates.erase(tabId);

    auto params = makeSessionParams(
        /*engineOptions=*/{},
        /*uiOptions=*/{},
        name,
        layoutName,
        wasVisible,
        tabId,
        std::move(enrichedSnapshot)
    );
    impl_->sessions.emplace_back(std::make_unique<Session>(std::move(params), std::move(proto)));
    if (wasVisible)
        setSelected(tabId);

    // Shut the old Session down after the swap.  Its snapshot has already
    // been handed off via the proto, so it owns nothing the new Session
    // depends on — the drain just walks DOM teardown and backend RPC
    // cleanup.  The lambda keeps it alive until shutdown completes.
    draining->shutdown([draining]() {
        Log::info("Preserved old Session drained after successful reconnect");
    });
}

void SessionArea::onProtoFailed(int tabId, std::string const& info)
{
    Log::warn("SessionArea::onProtoFailed: tabId={} info={}", tabId, info);
    auto rcIter = impl_->reconnectStates.find(tabId);
    if (rcIter == impl_->reconnectStates.end())
        return;
    rcIter->second.candidate.reset();
    scheduleReconnectRetry(tabId);
}

void SessionArea::abortReconnectCycle(int tabId)
{
    Log::info("SessionArea::abortReconnectCycle: tabId={}", tabId);
    cancelReconnectTimers(tabId);
    // Drop the proto before erasing the state; otherwise its destructor-
    // triggered FSM teardown could fire onFailed into a stale state.
    if (auto rcIter = impl_->reconnectStates.find(tabId); rcIter != impl_->reconnectStates.end())
        rcIter->second.candidate.reset();
    impl_->reconnectStates.erase(tabId);

    // Flip the old (preserved) Session's overlay back to its idle
    // [Reconnect] affordance.  The session itself is untouched — user can
    // re-click Reconnect to start another cycle.
    const auto sessionIdx = findSessionIndexByTabId(tabId);
    if (sessionIdx)
        impl_->sessions.value()[*sessionIdx]->stopReconnectUi();
}

void SessionArea::scheduleReconnectRetry(int tabId)
{
    auto rcIter = impl_->reconnectStates.find(tabId);
    if (rcIter == impl_->reconnectStates.end())
    {
        Log::warn("scheduleReconnectRetry: no reconnect state for tabId {}", tabId);
        return;
    }
    auto& reconnectState = rcIter->second;

    // Cap attempts against the session's max setting.  -1 means unlimited.
    int maxAttempts = -1;
    int maxBackoffMs = 16000;
    if (auto const* ssh = std::get_if<Persistence::SshSessionOptions>(&reconnectState.engineOptions.engine))
    {
        maxAttempts = ssh->maxReconnectAttempts;
        maxBackoffMs = ssh->maxReconnectBackoffMs;
    }

    if (maxAttempts >= 0 && reconnectState.attempt >= maxAttempts)
    {
        Log::warn(
            "scheduleReconnectRetry: reached max attempts ({}) for tabId {} — giving up",
            maxAttempts,
            tabId
        );
        abortReconnectCycle(tabId);
        return;
    }

    // Exponential backoff: 1s → 2s → 4s → 8s → 16s (capped at maxBackoffMs).
    const int clampedAttempt = std::min(reconnectState.attempt, 30); // prevent shift overflow
    const int delayMs = std::min(1000 << (clampedAttempt - 1), maxBackoffMs);
    const int initialSeconds = std::max(1, delayMs / 1000);

    // Drive the per-session overlay's attempt + countdown observables.
    const auto sessionIdx = findSessionIndexByTabId(tabId);
    if (sessionIdx)
    {
        auto& oldSession = impl_->sessions.value()[*sessionIdx];
        oldSession->setReconnectUiAttempt(reconnectState.attempt + 1);
        oldSession->setReconnectUiCountdown(initialSeconds);
    }

    // Tick the countdown once per second.  Stored as an opaque setTimeout
    // handle so cancelReconnectTimers can clear it on user Cancel or success.
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [this, tabId, tick, remaining = std::make_shared<int>(initialSeconds)]() mutable {
        auto rc = impl_->reconnectStates.find(tabId);
        if (rc == impl_->reconnectStates.end())
            return;
        (*remaining) -= 1;
        const auto sessionIdx = findSessionIndexByTabId(tabId);
        if (!sessionIdx)
            return;
        auto& sess = impl_->sessions.value()[*sessionIdx];
        if (*remaining <= 0)
        {
            sess->setReconnectUiCountdown(0);
            return;
        }
        sess->setReconnectUiCountdown(*remaining);
        rc->second.countdownTimerHandle = Nui::val::global("setTimeout")(
            Nui::bind(*tick), Nui::val{1000}
        );
    };
    reconnectState.countdownTimerHandle = Nui::val::global("setTimeout")(
        Nui::bind(*tick), Nui::val{1000}
    );

    // Schedule the actual retry firing after the full backoff.
    reconnectState.retryTimerHandle = Nui::val::global("setTimeout")(
        Nui::bind([this, tabId]() {
            auto rc = impl_->reconnectStates.find(tabId);
            if (rc == impl_->reconnectStates.end())
                return;
            const int nextAttempt = rc->second.attempt + 1;
            rc->second.retryTimerHandle = Nui::val::undefined();
            if (!rc->second.countdownTimerHandle.isUndefined())
            {
                Nui::val::global("clearTimeout")(rc->second.countdownTimerHandle);
                rc->second.countdownTimerHandle = Nui::val::undefined();
            }
            auto snapshotCopy = rc->second.snapshot;
            replaceAtTabId(tabId, std::move(snapshotCopy), nextAttempt);
        }),
        Nui::val{delayMs}
    );
}

void SessionArea::fireReconnectRetryNow(int tabId)
{
    auto rcIter = impl_->reconnectStates.find(tabId);
    if (rcIter == impl_->reconnectStates.end())
    {
        Log::warn("fireReconnectRetryNow: no reconnect state for tabId {}", tabId);
        return;
    }

    cancelReconnectTimers(tabId);

    const int nextAttempt = rcIter->second.attempt + 1;
    auto snapshotCopy = rcIter->second.snapshot;
    replaceAtTabId(tabId, std::move(snapshotCopy), nextAttempt);
}

void SessionArea::cancelReconnectTimers(int tabId)
{
    auto rc = impl_->reconnectStates.find(tabId);
    if (rc == impl_->reconnectStates.end())
        return;
    if (!rc->second.retryTimerHandle.isUndefined())
    {
        Nui::val::global("clearTimeout")(rc->second.retryTimerHandle);
        rc->second.retryTimerHandle = Nui::val::undefined();
    }
    if (!rc->second.countdownTimerHandle.isUndefined())
    {
        Nui::val::global("clearTimeout")(rc->second.countdownTimerHandle);
        rc->second.countdownTimerHandle = Nui::val::undefined();
    }
}

std::optional<nlohmann::json> SessionArea::getActiveSessionLayout()
{
    Session* activeSession = getActiveSession();
    if (activeSession)
        return activeSession->getLayout();
    return std::nullopt;
}

Session* SessionArea::getActiveSession()
{
    const auto selected = impl_->tabs.selectedId();
    if (selected == -1)
        return nullptr;

    for (auto const& session : impl_->sessions.value())
    {
        if (session->tabId() == selected)
            return session.get();
    }
    return nullptr;
}

Session* SessionArea::getSessionByLayoutId(std::string const& layoutId)
{
    for (auto const& session : impl_->sessions.value())
    {
        if (session->layoutId() == layoutId)
            return session.get();
    }
    return nullptr;
}

Nui::ElementRenderer SessionArea::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div; // because of the global div.

    Log::info("SessionArea::operator()");
    auto onExit = Nui::ScopeExit(
        []() noexcept
        {
            Log::info("SessionArea::operator() complete");
        }
    );

    try
    {
        // clang-format off
        return div{
            class_ = "session-area"
        }(
            impl_->tabs({
                "data-class"_attr = "session-area-tabs",
            }),
            div{
                style = "position: relative; width: 100%; height: calc(100% - 30px); display: block",
                class_ = "session-area-content"
            }(
                range(impl_->sessions),
                [this](long long i, auto& session) -> Nui::ElementRenderer {
                    Log::info("Rendering session '{}'", session->name());
                    Log::info("Session number {} of {}", i + 1, impl_->sessions.value().size());
                    return (*session)();
                }
            )
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in SessionArea::operator(): {}", e.what());
        return div{}("Error loading session area: "s + e.what());
    }
}