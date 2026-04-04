#include <frontend/terminal/frontend_session_manager.hpp>
#include <log/log.hpp>

struct FrontendSessionManager::Implementation
{
    std::unique_ptr<TerminalEngine> engine;
    std::unordered_map<Ids::ChannelId, std::unique_ptr<TerminalChannel>, Ids::IdHash> channels;
    bool beingDisposed{false};
    bool disposeComplete{false};
    bool isInLockedMode{false};
    std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput;

    /// Looks up the channel pointed to by a pending shared creation id.
    /// Returns nullptr if the id is not yet assigned or the channel is not found.
    TerminalChannel* findChannel(std::optional<Ids::ChannelId> const& id)
    {
        if (!id)
            return nullptr;
        auto found = channels.find(*id);
        return found != channels.end() ? found->second.get() : nullptr;
    }

    Implementation(
        std::unique_ptr<TerminalEngine> engine,
        std::function<void(Ids::ChannelId, std::string const&)> onLockedUserInput
    )
        : engine{std::move(engine)}
        , channels{}
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
    std::function<bool(Ids::ChannelId const&, TerminalChannel&)> const& handler
)
{
    if (guardDisposal())
        return;

    for (auto& [id, channel] : impl_->channels)
    {
        if (!handler(id, *channel))
            break;
    }
}

void FrontendSessionManager::connectionLossMode(bool active)
{
    if (guardDisposal())
        return;

    impl_->isInLockedMode = active;

    forEachChannel(
        [active](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.connectionLossMode(active);
            return true;
        }
    );
}

void FrontendSessionManager::broadcast(std::string const& msg)
{
    if (guardDisposal())
        return;

    forEachChannel(
        [&msg](Ids::ChannelId const&, TerminalChannel& channel) -> bool
        {
            channel.write(msg, false);
            return true;
        }
    );
}

void FrontendSessionManager::createChannel(
    Nui::val host,
    Persistence::TerminalOptions const& options,
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

    Log::info("Creating channel");

    // Shared so both the data-arrival callbacks and the creation callback can
    // reference the final channel id before it is known.
    auto channelId = std::make_shared<std::optional<Ids::ChannelId>>(std::nullopt);

    impl_->engine->createChannel(
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
        [this, channelId, onChannelCreated, host, options](
            std::optional<Ids::ChannelId> const& creationResult, std::string const& info
        )
        {
            if (!creationResult)
            {
                onChannelCreated(std::nullopt, info);
                return;
            }
            Log::info("Channel created.");

            *channelId = creationResult;

            [[maybe_unused]] auto [channelIter, _] = impl_->channels.emplace(
                **channelId,
                std::make_unique<TerminalChannel>(impl_->engine.get(), **channelId, impl_->onLockedUserInput)
            );
            if (channelIter == impl_->channels.end())
            {
                Log::error("Failed to create channel");
                onChannelCreated(std::nullopt, "Failed to create channel");
                return;
            }

            Log::info("Opening channel");
            channelIter->second->open(
                host,
                options,
                [onChannelCreated, channelId = **channelId, host](bool success, std::string const& info) mutable
                {
                    if (!success)
                    {
                        onChannelCreated(std::nullopt, info);
                        return;
                    }
                    host.call<void>("setAttribute", "data-channelid"s, channelId.value());
                    onChannelCreated(channelId, info);
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

void FrontendSessionManager::closeChannel(Ids::ChannelId const& channelId)
{
    if (guardDisposal())
        return;

    if (auto found = impl_->channels.find(channelId); found != impl_->channels.end())
    {
        Log::info("Closing channel: '{}'", channelId.value());
        impl_->channels.erase(found);
    }
}

void FrontendSessionManager::closeAllChannels()
{
    if (guardDisposal())
        return;

    Log::info("Closing all channels");
    impl_->channels.clear();
}

TerminalEngine& FrontendSessionManager::engine()
{
    return *impl_->engine;
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
    impl_->engine->open(
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

    // Don't close backend channels individually — they are cleaned up by the
    // engine's own dispose, which tears down the entire transport.
    for (auto& [id, channel] : impl_->channels)
    {
        Log::info("Disposing channel as part of frontend ssh manager dispose: '{}'", id.value());
        channel->dispose(
            []()
            {
                Log::info("Disposed channel as part of frontend ssh manager dispose");
            },
            false
        );
    }

    Log::info("No more channels to dispose, disposing engine.");
    impl_->engine->dispose(
        [this, onComplete = std::move(onComplete)]()
        {
            Log::info("Frontend ssh manager disposed.");
            impl_->disposeComplete = true;
            onComplete();
        }
    );
}

FrontendSessionManager::~FrontendSessionManager()
{
    dispose([]() {});
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(FrontendSessionManager);
