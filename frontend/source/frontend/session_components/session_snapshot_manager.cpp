#include <frontend/session_components/session_snapshot_manager.hpp>

#include <frontend/session_components/terminal_panel.hpp>
#include <frontend/session_components/file_explorer_panel.hpp>
#include <frontend/session_components/operation_queue.hpp>
#include <frontend/session_components/session_layout_initializer.hpp>
#include <frontend/terminal/frontend_session_manager.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <log/log.hpp>

#include <nui-file-explorer/side.hpp>

#include <nui/frontend/rpc_client.hpp>
#include <nui/frontend/val.hpp>

#include <unordered_map>
#include <utility>

using namespace Nui;

struct SessionSnapshotManager::Implementation
{
    Nui::Observed<std::unique_ptr<FrontendSessionManager>>* frontendSessionManager;
    TerminalPanel* terminalPanel;
    FileExplorerPanel* fileExplorerPanel;
    OperationQueue* operationQueue;
    SessionLayoutInitializer* layoutInitializer;

    std::optional<SessionSnapshot> pendingResumeSnapshot;

    /**
     * @brief Bulk OperationIds whose backend resume backups this Session must
     *        clean up.  Destructor discards any that survive; adoption
     *        normally removes them server-side first.
     */
    std::vector<Ids::OperationId> trackedBulkResumes;

    /** @brief Consumed by localShellFactory during layout restore on reconnect. */
    std::vector<LocalShellAdoption> pendingLocalShellAdoptions;

    /** @brief Primary-channel scrollback dumps awaiting replay by onOpenChannel. */
    std::deque<std::string> pendingScrollbackReplay;

    /**
     * @brief Lumino layout saved out of pendingResumeSnapshot up front so the
     *        DOM-attach-triggered initialize still finds it after the
     *        snapshot is reset.
     */
    nlohmann::json pendingResumeLayout{};

    /** @brief Cached remote username so the tab title survives reconnect. */
    std::string remoteUsername;
    bool sftpIsOpen{false};

    explicit Implementation(Params&& params)
        : frontendSessionManager{params.frontendSessionManager}
        , terminalPanel{params.terminalPanel}
        , fileExplorerPanel{params.fileExplorerPanel}
        , operationQueue{params.operationQueue}
        , layoutInitializer{params.layoutInitializer}
        , pendingResumeSnapshot{std::move(params.initialPending)}
    {
        // Seed adoptions up front: layout restore can race ahead of
        // onOpenSession's apply, and localShellFactory needs them ready.
        if (pendingResumeSnapshot.has_value())
            pendingLocalShellAdoptions = pendingResumeSnapshot->ejectedLocalShells;
    }
};

SessionSnapshotManager::SessionSnapshotManager(Params params)
    : impl_{std::make_unique<Implementation>(std::move(params))}
{}
SessionSnapshotManager::~SessionSnapshotManager()
{
    // Discard tracked backend resume backups.  No-op in the happy path
    // (adoption consumes them server-side); matters if the tab closes
    // without reaching apply or adoptBulkResume failed.
    if (!impl_ || impl_->trackedBulkResumes.empty())
        return;

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
SessionSnapshotManager::SessionSnapshotManager(SessionSnapshotManager&&) = default;
SessionSnapshotManager& SessionSnapshotManager::operator=(SessionSnapshotManager&&) = default;

SessionSnapshot SessionSnapshotManager::capture(bool withEjection)
{
    SessionSnapshot out;

    // Primary SSH channel scrollback, in FSM iteration order.  Must be read
    // BEFORE the FSM is disposed getAllTextContent goes through the xterm
    // serializeAddon, which lives inside the terminal DOM element.
    if (impl_->frontendSessionManager->value())
    {
        impl_->frontendSessionManager->value()->forEachChannel(
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
    out.local = captureSide(impl_->fileExplorerPanel->localFileGridSide());
    if (auto* remote = impl_->fileExplorerPanel->remoteFileGridSide(); remote)
        out.remote = captureSide(*remote);

    out.remoteUsername = impl_->remoteUsername;
    out.sftpWasOpen = impl_->sftpIsOpen;
    out.inFlightOps = impl_->operationQueue->snapshotInFlight();
    Log::info("capture: captured {} in-flight operation(s)", out.inFlightOps.size());

    // Skip ejection when called non-destructively (ProtoSession flow);
    // ejectLocalShellsForHandoff runs at the swap moment instead.
    if (withEjection)
        out.ejectedLocalShells = ejectLocalShellsForHandoff();

    // Lumino layout reuses the same serialisation getLayout() produces for
    // layout persistence, so the reconnect restore is byte-compatible.
    if (impl_->layoutInitializer)
    {
        if (auto layout = impl_->layoutInitializer->getLayout(); layout)
        {
            Log::info("capture: Lumino layout captured ({} chars): {}.", layout->dump().size(), layout->dump());
            out.luminoLayout = std::move(*layout);
        }
        else
        {
            Log::warn("capture: layoutInitializer->getLayout() returned nullopt.");
        }
    }
    else
    {
        Log::warn("capture: no layoutInitializer wired, Lumino layout not captured.");
    }

    return out;
}

std::vector<LocalShellAdoption> SessionSnapshotManager::ejectLocalShellsForHandoff()
{
    std::vector<LocalShellAdoption> out;
    if (!impl_->frontendSessionManager->value())
        return out;

    // Dump scrollback first: ejection destroys the frontend ExecutingChannel
    // but leaves the backend process alive for adoption.
    std::unordered_map<Ids::ChannelId, std::string, Ids::IdHash> auxScrollback;
    impl_->frontendSessionManager->value()->forEachChannel(
        FrontendSessionManager::EngineFilter::LocalShellOnly,
        [&auxScrollback](Ids::ChannelId const& channelId, TerminalChannel& channel) {
            auxScrollback.emplace(channelId, channel.getAllTextContent());
            return true;
        }
    );

    auto* auxEngine = impl_->frontendSessionManager->value()->auxiliaryLocalShellEngine();
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
                "Ejected local-shell '{}' has no tracked metadata dropping",
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

void SessionSnapshotManager::apply(SessionSnapshot const& snapshot)
{
    // File-explorer side prefs apply before navigation so the first listing
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
    applySide(impl_->fileExplorerPanel->localFileGridSide(), snapshot.local);
    if (auto* remote = impl_->fileExplorerPanel->remoteFileGridSide(); remote)
        applySide(*remote, snapshot.remote);

    // Queue scrollback for replay as primary channels open; channels don't
    // exist yet at apply time.  replayScrollbackFor drains the front entry
    // per successfully-opened primary channel.
    impl_->pendingScrollbackReplay.clear();
    for (auto const& dump : snapshot.primaryChannelScrollback)
        impl_->pendingScrollbackReplay.push_back(dump);

    // Single-file ops go through OperationQueue directly; bulk ops are
    // adopted server-side via SessionManager::adoptBulkResume.
    // OperationIds are tracked for destructor-time cleanup.
    Log::info(
        "apply: replaying {} in-flight operation(s) from snapshot",
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
                impl_->operationQueue->enqueueResumable(op);
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
    if (!bulkResumeIds.empty() && impl_->frontendSessionManager->value() &&
        impl_->frontendSessionManager->value()->engine().engineName() == "ssh")
    {
        auto* sshTerminalEngine =
            static_cast<SshTerminalEngine*>(&impl_->frontendSessionManager->value()->engine());
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

    // pendingLocalShellAdoptions was seeded in the ctor; layout restore may
    // have already consumed entries, so don't overwrite with the raw list.
}

std::optional<SessionSnapshot> SessionSnapshotManager::takePendingSnapshot()
{
    if (!impl_->pendingResumeSnapshot)
        return std::nullopt;
    auto out = std::move(*impl_->pendingResumeSnapshot);
    impl_->pendingResumeSnapshot.reset();
    return out;
}

bool SessionSnapshotManager::hasPending() const
{
    return impl_->pendingResumeSnapshot.has_value();
}

SessionSnapshot const& SessionSnapshotManager::pending() const
{
    return *impl_->pendingResumeSnapshot;
}

void SessionSnapshotManager::resetPending()
{
    impl_->pendingResumeSnapshot.reset();
}

void SessionSnapshotManager::setLayoutInitializer(SessionLayoutInitializer* layoutInitializer)
{
    impl_->layoutInitializer = layoutInitializer;
}

std::optional<nlohmann::json> SessionSnapshotManager::takeResumeLayout()
{
    if (!impl_->pendingResumeLayout.is_null())
    {
        auto layout = std::move(impl_->pendingResumeLayout);
        impl_->pendingResumeLayout = nlohmann::json{};
        Log::info("takeResumeLayout: returning seeded layout ({} chars): {}.", layout.dump().size(), layout.dump());
        return layout;
    }
    if (impl_->pendingResumeSnapshot && !impl_->pendingResumeSnapshot->luminoLayout.is_null())
    {
        Log::info(
            "takeResumeLayout: returning snapshot's luminoLayout ({} chars): {}.",
            impl_->pendingResumeSnapshot->luminoLayout.dump().size(),
            impl_->pendingResumeSnapshot->luminoLayout.dump()
        );
        return impl_->pendingResumeSnapshot->luminoLayout;
    }
    Log::info("takeResumeLayout: no layout available, returning nullopt.");
    return std::nullopt;
}

void SessionSnapshotManager::seedPendingResumeLayout(nlohmann::json layout)
{
    Log::info("seedPendingResumeLayout: seeding layout ({} chars): {}.", layout.dump().size(), layout.dump());
    impl_->pendingResumeLayout = std::move(layout);
}

std::vector<LocalShellAdoption>* SessionSnapshotManager::pendingLocalShellAdoptionsPtr()
{
    return &impl_->pendingLocalShellAdoptions;
}

void SessionSnapshotManager::replayScrollbackFor(Ids::ChannelId const& channelId, TerminalChannel& channel)
{
    if (impl_->pendingScrollbackReplay.empty())
        return;
    channel.replayContent(impl_->pendingScrollbackReplay.front());
    impl_->pendingScrollbackReplay.pop_front();
    (void)channelId;
}

std::string const& SessionSnapshotManager::remoteUsername() const
{
    return impl_->remoteUsername;
}

void SessionSnapshotManager::setRemoteUsername(std::string value)
{
    impl_->remoteUsername = std::move(value);
}

bool SessionSnapshotManager::sftpIsOpen() const
{
    return impl_->sftpIsOpen;
}

void SessionSnapshotManager::setSftpIsOpen(bool value)
{
    impl_->sftpIsOpen = value;
}
