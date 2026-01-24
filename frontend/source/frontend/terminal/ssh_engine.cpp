#include <frontend/terminal/ssh_engine.hpp>
#include <frontend/nlohmann_compat.hpp>
#include <log/log.hpp>

#include <nui/utility/scope_exit.hpp>
#include <nui/frontend/val.hpp>
#include <nui/rpc.hpp>

using namespace std::string_literals;

struct SshTerminalEngine::Implementation
{
    SshTerminalEngine::Settings settings;
    Ids::SessionId sshSessionId;
    std::unordered_map<Ids::ChannelId, SshChannel, Ids::IdHash> channels;
    Nui::RpcClient::AutoUnregister onDisconnectReceiver_;
    bool wasDisposed;
    bool blockedByDestruction;

    Implementation(SshTerminalEngine::Settings&& settings)
        : settings{std::move(settings)}
        , sshSessionId{}
        , channels{}
        , onDisconnectReceiver_{}
        , wasDisposed{false}
        , blockedByDestruction{false}
    {}
};

SshTerminalEngine::SshTerminalEngine(Settings settings)
    : impl_{std::make_unique<Implementation>(std::move(settings))}
{}
SshTerminalEngine::~SshTerminalEngine()
{
    if (!moveDetector_.wasMoved() && !impl_->wasDisposed)
    {
        // Does not do channel close chain, but quick-kills the session.
        Log::info("Disconnecting from destructor");
        disconnect([]() {});
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(SshTerminalEngine);

void SshTerminalEngine::open(std::function<void(bool, std::string const&)> onOpen)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("Blocked by destruction");
        return onOpen(false, "Blocked by destruction");
    }

    Nui::val obj = Nui::val::object();
    obj.set("engineOptions", asVal(impl_->settings.engineOptions));

    Nui::RpcClient::callWithBackChannel(
        "SessionManager::connect",
        [this, onOpen = std::move(onOpen)](Nui::val val)
        {
            if (!val.hasOwnProperty("id"))
            {
                Log::error("SessionManager::connect callback did not return an id");
                std::string error = "";
                if (val.hasOwnProperty("error"))
                    error = val["error"].as<std::string>();
                return onOpen(false, error);
            }
            impl_->sshSessionId = Ids::makeSessionId(val["id"].as<std::string>());
            onSuccessfulOpen();
            onOpen(true, "");
        },
        obj
    );
}

void SshTerminalEngine::onSuccessfulOpen()
{
    impl_->onDisconnectReceiver_ = Nui::RpcClient::autoRegisterFunction(
        fmt::format("Session::{}::onDisconnect", impl_->sshSessionId.value()),
        [this](Nui::val)
        {
            Log::info("SSH session '{}' disconnected", impl_->sshSessionId.value());
            disconnect([]() {}, true);
        }
    );
}

void SshTerminalEngine::disconnect(std::function<void()> onDisconnect, bool byLossOfConnection)
{
    if (impl_->wasDisposed)
        return onDisconnect();

    impl_->wasDisposed = true;
    Log::info("Disconnecting session: {}", impl_->sshSessionId.value());
    if (!byLossOfConnection)
    {
        Nui::RpcClient::callWithBackChannel(
            "SessionManager::disconnect",
            [onDisconnect = std::move(onDisconnect)](Nui::val)
            {
                // TODO: handle error
                Log::info("Frontend SshEngine: Disconnected");
                onDisconnect();
            },
            impl_->sshSessionId.value()
        );
    }
    else
    {
        Log::info("Frontend SshEngine: Disconnected by loss of connection.");
        onDisconnect();
        if (impl_->settings.onConnectionLoss)
            impl_->settings.onConnectionLoss();
    }
}

void SshTerminalEngine::createChannelImpl(
    std::function<void(std::string const&)> handler,
    std::function<void(std::string const&)> errorHandler,
    std::function<void(std::optional<Ids::ChannelId> const&)> onCreated,
    bool fileMode
)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("Blocked by destruction");
        return onCreated(std::nullopt);
    }

    Nui::val obj = Nui::val::object();
    obj.set("engineOptions", asVal(impl_->settings.engineOptions));
    obj.set("fileMode", fileMode);

    Log::info("Creating {} channel for session '{}'", fileMode ? "sftp" : "pty", impl_->sshSessionId.value());
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::create", impl_->sshSessionId.value()),
        [this,
            onCreated = std::move(onCreated),
            handler = std::move(handler),
            errorHandler = std::move(errorHandler),
            fileMode](Nui::val val)
        {
            if (val.hasOwnProperty("error"))
            {
                Log::error("Failed to create channel: {}", val["error"].as<std::string>());
                onCreated(std::nullopt);
                return;
            }

            if (!val.hasOwnProperty("id"))
            {
                Log::error("Session::Channel::create callback did not return an id");
                onCreated(std::nullopt);
                return;
            }

            const auto channelId = Ids::makeChannelId(val["id"].as<std::string>());
            [[maybe_unused]] const auto [iter, _] =
                impl_->channels.emplace(channelId, SshChannel{impl_->sshSessionId, channelId});

            // Creates recepticals for stdout/stderr
            iter->second.open(handler, errorHandler, fileMode);

            if (!fileMode)
            {
                Nui::RpcClient::callWithBackChannel(
                    fmt::format("Session::{}::Channel::startReading", impl_->sshSessionId.value()),
                    [this, channelId, onCreated](Nui::val val)
                    {
                        if (val.hasOwnProperty("error"))
                        {
                            Log::error("Failed to start reading: {}", val["error"].as<std::string>());
                            closeChannel(channelId, []() {});
                            onCreated(std::nullopt);
                            return;
                        }
                        Log::info("Started reading: {}", channelId.value());
                        onCreated(channelId);
                    },
                    channelId.value()
                );
            }
            else
            {
                onCreated(channelId);
            }
        },
        obj
    );
}

void SshTerminalEngine::createChannel(
    std::function<void(std::string const&)> handler,
    std::function<void(std::string const&)> errorHandler,
    std::function<void(std::optional<Ids::ChannelId> const&)> onCreated
)
{
    createChannelImpl(std::move(handler), std::move(errorHandler), std::move(onCreated), false);
}

void SshTerminalEngine::createSftpChannel(std::function<void(std::optional<Ids::ChannelId> const&)> onCreated)
{
    createChannelImpl([](std::string const&) {}, [](std::string const&) {}, std::move(onCreated), true);
}

Ids::SessionId SshTerminalEngine::sshSessionId() const
{
    return impl_->sshSessionId;
}

void SshTerminalEngine::dispose(std::function<void()> onDisposeComplete)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("SshTerminalEngine, dispose already in progress");
        return;
    }
    Log::info("Disposing SshTerminalEngine");
    impl_->blockedByDestruction = true;
    // dont destroy channels, host process already cleans up
    disconnect(std::move(onDisposeComplete));
}

void SshTerminalEngine::closeChannel(Ids::ChannelId const& channelId, std::function<void()> onChannelClosed)
{
    Log::info("SshTerminalEngine: Closing channel: {}", channelId.value());
    if (auto channel = impl_->channels.find(channelId); channel != impl_->channels.end())
    {
        channel->second.dispose(
            [this, channelId, onChannelClosed = std::move(onChannelClosed)]()
            {
                impl_->channels.erase(channelId);
                onChannelClosed();
            }
        );
    }
    else
    {
        Log::error("Failed to close channel (could not find it): {}", channelId.value());
        onChannelClosed();
    }
}
SshChannel* SshTerminalEngine::channel(Ids::ChannelId const& channelId)
{
    if (auto channel = impl_->channels.find(channelId); channel != impl_->channels.end())
    {
        return &channel->second;
    }
    return nullptr;
}