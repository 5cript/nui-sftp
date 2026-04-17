#include <frontend/terminal/frontend_session_manager.hpp>
#include <log/log.hpp>

#include <unordered_map>

struct FrontendSessionManager::Implementation
{
    std::unique_ptr<TerminalEngine> primaryEngine;
    std::unique_ptr<ExecutingTerminalEngine> auxLocalShellEngine;
    std::unordered_map<Ids::ChannelId, std::unique_ptr<TerminalChannel>, Ids::IdHash> channels;
    // Owner engine for each channel — used to filter connection-loss / broadcast /
    // disposal operations so local-shell channels stay isolated from SSH transport
    // events.
    std::unordered_map<Ids::ChannelId, TerminalEngine*, Ids::IdHash> channelEngine;
    bool beingDisposed{false};
    bool disposeComplete{false};
    bool isInLockedMode{false};
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput;

    /// Looks up the channel pointed to by a pending shared creation id.
    /// Returns nullptr if the id is not yet assigned or the channel is not found.
    TerminalChannel* findChannel(std::optional<Ids::ChannelId> const& chId)
    {
        if (!chId)
            return nullptr;
        auto found = channels.find(*chId);
        return found != channels.end() ? found->second.get() : nullptr;
    }

    Implementation(
        std::unique_ptr<TerminalEngine> primaryEngine,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : primaryEngine{std::move(primaryEngine)}
        , auxLocalShellEngine{}
        , channels{}
        , channelEngine{}
        , onLockedUserInput{std::move(onLockedUserInput)}
    {}
};

FrontendSessionManager::FrontendSessionManager(
    std::unique_ptr<TerminalEngine> engine,
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
)
    : impl_{std::make_unique<Implementation>(std::move(engine), std::move(onLockedUserInput))}
{}

bool FrontendSessionManager::guardDisposal() const
{
    if (impl_->beingDisposed)
    {
        Log::warn("Cannot perform operation, frontend ssh manager is being disposed");
        return true;
    }
    return false;
}

void FrontendSessionManager::forEachChannel(
    EngineFilter filter,
    std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler
)
{
    if (guardDisposal())
        return;

    for (auto& [channelId, channel] : impl_->channels)
    {
        if (filter != EngineFilter::AllChannels)
        {
            auto found = impl_->channelEngine.find(channelId);
            const bool isLocalShell =
                found != impl_->channelEngine.end()
                && impl_->auxLocalShellEngine
                && found->second == impl_->auxLocalShellEngine.get();
            if (filter == EngineFilter::PrimaryOnly && isLocalShell)
                continue;
            if (filter == EngineFilter::LocalShellOnly && !isLocalShell)
                continue;
        }

        if (!handler(channelId, *channel))
            break;
    }
}

void FrontendSessionManager::forEachChannel(
    std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler
)
{
    forEachChannel(EngineFilter::AllChannels, handler);
}

void FrontendSessionManager::connectionLossMode(bool active, EngineFilter filter)
{
    if (guardDisposal())
        return;

    // Only primary-engine channels flip global locked state — local shells
    // don't care about the SSH transport.
    if (filter == EngineFilter::PrimaryOnly || filter == EngineFilter::AllChannels)
        impl_->isInLockedMode = active;

    forEachChannel(
        filter,
        [active](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.connectionLossMode(active);
            return true;
        }
    );
}

void FrontendSessionManager::broadcast(std::string const& msg, EngineFilter filter)
{
    if (guardDisposal())
        return;

    forEachChannel(
        filter,
        [&msg](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.write(msg, false);
            return true;
        }
    );
}

namespace
{
    // Shared create-channel body used by both primary and aux-engine paths.
    // Wires the engine-agnostic handlers to a TerminalChannel in the manager's
    // map, tagged with @p ownerEngine for filtering later.
    struct CreateChannelContext
    {
        TerminalEngine* ownerEngine;
        Nui::val host;
        Persistence::TerminalOptions const& terminalOptions;
    };
} // namespace

void FrontendSessionManager::createChannel(
    Nui::val host,
    Persistence::TerminalOptions const& options,
    ChannelCreationOptions const& channelOptions,
    std::function<void(std::optional<Ids::ChannelId>, std::string const& info)> onChannelCreated,
    std::function<void(Ids::ChannelId const&)> onChannelLoss
)
{
    if (guardDisposal())
        return;

    using namespace std::string_literals;

    if (impl_->isInLockedMode)
    {
        Log::warn("Cannot create channel, frontend ssh manager is in locked mode");
        onChannelCreated(std::nullopt, "Cannot create channel, connection is lost");
        return;
    }

    Log::info("Creating channel on primary engine");

    // Shared so both the data-arrival callbacks and the creation callback can
    // reference the final channel id before it is known.
    auto channelId = std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);
    auto* primary = impl_->primaryEngine.get();

    primary->createChannel(
        channelOptions,
        [this, channelId](std::string const& data)
        {
            if (auto* channel = impl_->findChannel(*channelId))
                channel->write(data, false);
        },
        [this, channelId](std::string const& data)
        {
            if (auto* channel = impl_->findChannel(*channelId))
                channel->writeStderr(data, false);
        },
        [this, channelId, onChannelCreated, host, options, primary](
            std::optional<Ids::ChannelId> const& creationResult, std::string const& info
        )
        {
            if (!creationResult)
            {
                onChannelCreated(std::nullopt, info);
                return;
            }
            Log::info("Primary channel created.");

            *channelId = creationResult;

            [[maybe_unused]] auto [channelIter, _] = impl_->channels.emplace(
                **channelId,
                std::make_unique<TerminalChannel>(primary, **channelId, impl_->onLockedUserInput)
            );
            if (channelIter == impl_->channels.end())
            {
                Log::error("Failed to create channel");
                onChannelCreated(std::nullopt, "Failed to create channel");
                return;
            }
            impl_->channelEngine[**channelId] = primary;

            Log::info("Opening channel");
            channelIter->second->open(
                host,
                options,
                [onChannelCreated, chId = **channelId, host](bool success, std::string const& info) mutable
                {
                    if (!success)
                    {
                        onChannelCreated(std::nullopt, info);
                        return;
                    }
                    host.call<void>("setAttribute", "data-channelid"s, chId.value());
                    onChannelCreated(chId, info);
                }
            );
        },
        [this, onChannelLoss](Ids::ChannelId const& lostChannelId)
        {
            Log::info("Channel lost: '{}'", lostChannelId.value());
            onChannelLoss(lostChannelId);
            // In locked mode the terminal stays visible so the user can save its
            // contents or press Enter to close.  Destroying the TerminalChannel
            // here would leave the xterm onData callback with a dangling this
            // pointer.  dispose() will clean it up when the session actually closes.
            if (!impl_->isInLockedMode)
                closeChannel(lostChannelId);
        }
    );
}

void FrontendSessionManager::createLocalShellChannel(
    Nui::val host,
    Persistence::TerminalOptions const& terminalOptions,
    Persistence::ExecutingSessionOptions const& shellOptions,
    Persistence::Termios const& termios,
    std::function<void(Ids::ChannelId const&, std::string)> onProcessChange,
    std::function<void(std::optional<Ids::ChannelId>, std::string const& info)> onChannelCreated,
    std::function<void(Ids::ChannelId const&)> onChannelLoss
)
{
    if (guardDisposal())
        return;

    using namespace std::string_literals;

    // Lazy-create the aux engine. First caller wires onProcessChange; subsequent
    // calls reuse the same engine (single-aux-engine design — see plan).
    if (!impl_->auxLocalShellEngine)
    {
        Log::info("Lazily constructing aux local-shell engine");
        impl_->auxLocalShellEngine = std::make_unique<ExecutingTerminalEngine>(
            ExecutingTerminalEngine::Settings{.onProcessChange = std::move(onProcessChange)}
        );
        impl_->auxLocalShellEngine->open([](bool, std::string const&) {});
    }

    Log::info("Creating channel on aux local-shell engine");

    ExecutingChannelCreationOptions execOptions;
    execOptions.executingOptions = shellOptions;
    execOptions.termios = termios;

    auto channelId = std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);
    auto* aux = impl_->auxLocalShellEngine.get();

    aux->createChannel(
        execOptions,
        [this, channelId](std::string const& data)
        {
            if (auto* channel = impl_->findChannel(*channelId))
                channel->write(data, false);
        },
        [this, channelId](std::string const& data)
        {
            if (auto* channel = impl_->findChannel(*channelId))
                channel->writeStderr(data, false);
        },
        [this, channelId, onChannelCreated, host, terminalOptions, aux](
            std::optional<Ids::ChannelId> const& creationResult, std::string const& info
        )
        {
            if (!creationResult)
            {
                onChannelCreated(std::nullopt, info);
                return;
            }
            Log::info("Local-shell channel created.");

            *channelId = creationResult;

            [[maybe_unused]] auto [channelIter, _] = impl_->channels.emplace(
                **channelId,
                std::make_unique<TerminalChannel>(aux, **channelId, impl_->onLockedUserInput)
            );
            if (channelIter == impl_->channels.end())
            {
                Log::error("Failed to create local-shell channel");
                onChannelCreated(std::nullopt, "Failed to create local-shell channel");
                return;
            }
            impl_->channelEngine[**channelId] = aux;

            Log::info("Opening local-shell channel");
            channelIter->second->open(
                host,
                terminalOptions,
                [onChannelCreated, chId = **channelId, host](bool success, std::string const& info) mutable
                {
                    if (!success)
                    {
                        onChannelCreated(std::nullopt, info);
                        return;
                    }
                    host.call<void>("setAttribute", "data-channelid"s, chId.value());
                    onChannelCreated(chId, info);
                }
            );
        },
        [this, onChannelLoss](Ids::ChannelId const& lostChannelId)
        {
            Log::info("Local-shell channel lost (process exit): '{}'", lostChannelId.value());
            onChannelLoss(lostChannelId);
            // A process exit is not a transport loss — always tear down the
            // TerminalChannel regardless of the SSH connection-loss flag.
            closeChannel(lostChannelId);
        }
    );
}

TerminalChannel* FrontendSessionManager::channel(Ids::ChannelId const& channelId)
{
    if (impl_->beingDisposed)
    {
        Log::warn("Cannot get channel, frontend ssh manager is being disposed");
        return nullptr;
    }

    Log::debug("Getting channel: '{}'", channelId.value());
    auto found = impl_->channels.find(channelId);
    return found != impl_->channels.end() ? found->second.get() : nullptr;
}

TerminalEngine* FrontendSessionManager::engineOf(Ids::ChannelId const& channelId)
{
    if (impl_->beingDisposed)
        return nullptr;
    auto found = impl_->channelEngine.find(channelId);
    return found != impl_->channelEngine.end() ? found->second : nullptr;
}

bool FrontendSessionManager::isLocalShellChannel(Ids::ChannelId const& channelId)
{
    if (!impl_->auxLocalShellEngine)
        return false;
    return engineOf(channelId) == impl_->auxLocalShellEngine.get();
}

void FrontendSessionManager::closeChannel(Ids::ChannelId const& channelId)
{
    if (guardDisposal())
        return;

    if (auto found = impl_->channels.find(channelId); found != impl_->channels.end())
    {
        Log::info("Closing channel: '{}'", channelId.value());
        found->second->dispose(
            [this, channelId]()
            {
                impl_->channels.erase(channelId);
                impl_->channelEngine.erase(channelId);
            },
            true // also close backend resources immediately
        );
    }
}

void FrontendSessionManager::closeAllChannels()
{
    if (guardDisposal())
        return;

    Log::info("Closing all channels");
    for (auto& [channelId, channel] : impl_->channels)
    {
        Log::info("Disposing channel as part of closeAllChannels: '{}'", channelId.value());
        channel->dispose(
            []()
            {
                Log::info("Disposed channel as part of closeAllChannels");
            },
            true // also close backend resources immediately
        );
    }
    impl_->channels.clear();
    impl_->channelEngine.clear();
}

TerminalEngine& FrontendSessionManager::engine()
{
    return *impl_->primaryEngine;
}

void FrontendSessionManager::focusFirst()
{
    if (guardDisposal())
        return;

    if (!impl_->channels.empty())
        impl_->channels.begin()->second->focus();
}

void FrontendSessionManager::open(std::function<void(bool, std::string const&)> onOpen)
{
    if (guardDisposal())
        return;

    Log::info("Opening session");
    impl_->primaryEngine->open(
        [this, onOpen = std::move(onOpen)](bool success, std::string const& info)
        {
            if (!success)
            {
                Log::error("Failed to open terminal: '{}'", info);
                dispose(
                    [onOpen, info]()
                    {
                        onOpen(false, info);
                    }
                );
                return;
            }
            onOpen(true, info);
        }
    );
}

void FrontendSessionManager::dispose(std::function<void()> onComplete)
{
    if (impl_->disposeComplete)
    {
        onComplete();
        return;
    }

    if (impl_->beingDisposed)
    {
        Log::warn("Frontend ssh manager, dispose already in progress");
        onComplete();
        return;
    }

    impl_->beingDisposed = true;

    // Don't close backend channels individually — they are cleaned up by each
    // engine's own dispose, which tears down the transport (SSH) or terminates
    // each child process (Executing).
    for (auto& [channelId, channel] : impl_->channels)
    {
        Log::info("Disposing channel as part of frontend ssh manager dispose: '{}'", channelId.value());
        channel->dispose(
            []()
            {
                Log::info("Disposed channel as part of frontend ssh manager dispose");
            },
            false
        );
    }

    // Compose: primary engine first, then aux (if present).
    auto finalStep = [this, onComplete = std::move(onComplete)]()
    {
        Log::info("Frontend ssh manager disposed.");
        impl_->disposeComplete = true;
        onComplete();
    };

    auto disposeAux = [this, finalStep = std::move(finalStep)]() mutable
    {
        if (!impl_->auxLocalShellEngine)
        {
            finalStep();
            return;
        }
        Log::info("Disposing aux local-shell engine.");
        impl_->auxLocalShellEngine->dispose(
            [finalStep = std::move(finalStep)]()
            {
                Log::info("Aux local-shell engine disposed.");
                finalStep();
            }
        );
    };

    Log::info("Disposing primary engine.");
    impl_->primaryEngine->dispose(std::move(disposeAux));
}

FrontendSessionManager::~FrontendSessionManager()
{
    dispose([]() {});
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(FrontendSessionManager);
