#include <frontend/terminal/ftp_engine.hpp>
#include <frontend/terminal/remote_engine_factory.hpp>
#include <frontend/nlohmann_compat.hpp>
#include <log/log.hpp>

#include <fmt/format.h>

#include <nui/frontend/val.hpp>
#include <nui/rpc.hpp>

#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
    /**
     * @brief The error of a backend reply, or @p fallback when the reply names none.
     */
    std::string replyError(Nui::val const& reply, std::string_view fallback)
    {
        if (reply.hasOwnProperty("error"))
            return reply["error"].as<std::string>();
        return std::string{fallback};
    }

    /**
     * @brief Ends a session whose connect finished after its engine was gone.
     */
    void disconnectOrphan(Ids::SessionId const& sessionId)
    {
        Log::info("Disconnecting FTP session '{}', its engine is gone", sessionId.value());
        Nui::val parameters = Nui::val::object();
        parameters.set("sessionId", sessionId.value());
        Nui::RpcClient::callWithBackChannel("SessionManager::disconnect", [](Nui::val) {}, parameters);
    }
}

struct FtpTerminalEngine::Implementation
{
    Settings settings;
    Ids::SessionId sessionId{};
    std::unordered_set<Ids::ChannelId, Ids::IdHash> fileChannels{};
    Nui::RpcClient::AutoUnregister onDisconnectReceiver{};
    bool wasDisposed{false};
    bool blockedByDestruction{false};

    explicit Implementation(Settings&& settings)
        : settings{std::move(settings)}
    {}
};

FtpTerminalEngine::FtpTerminalEngine(Settings settings)
    : impl_{std::make_unique<Implementation>(std::move(settings))}
{}

FtpTerminalEngine::~FtpTerminalEngine()
{
    if (!moveDetector_.wasMoved() && !impl_->wasDisposed)
    {
        Log::info("Disconnecting the FTP session from the destructor");
        disconnect([]() {}, false);
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(FtpTerminalEngine);

void FtpTerminalEngine::open(std::function<void(EngineOpenResult, std::string const&)> onOpen, Log::Toast failureToast)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("Blocked by destruction");
        return onOpen(EngineOpenResult::Failed, "Blocked by destruction");
    }

    Nui::val parameters = Nui::val::object();
    parameters.set("engineType", std::string{"ftp"});
    parameters.set("engineOptions", asVal(impl_->settings.options));
    parameters.set("showFailureToast", failureToast == Log::Toast::Show);

    Nui::RpcClient::callWithBackChannel(
        "SessionManager::connect",
        [this, alive = lifetime_.watch(), onOpen = std::move(onOpen)](Nui::val reply)
        {
            if (alive.expired() || impl_->wasDisposed)
            {
                if (reply.hasOwnProperty("id"))
                    disconnectOrphan(Ids::makeSessionId(reply["id"].as<std::string>()));
                return;
            }
            if (!reply.hasOwnProperty("id"))
            {
                // The backend logged the reason with failureToast already.
                const auto error = replyError(reply, "The FTP connect returned no session");
                Log::error(Log::Toast::Skip, "FTP connect failed: {}", error);
                return onOpen(connectFailureOf(reply), error);
            }
            impl_->sessionId = Ids::makeSessionId(reply["id"].as<std::string>());
            onSuccessfulOpen();
            onOpen(EngineOpenResult::Opened, "");
        },
        parameters
    );
}

void FtpTerminalEngine::onSuccessfulOpen()
{
    impl_->onDisconnectReceiver = Nui::RpcClient::autoRegisterFunction(
        fmt::format("Session::{}::onDisconnect", impl_->sessionId.value()),
        [this](Nui::val)
        {
            Log::info("FTP session '{}' disconnected", impl_->sessionId.value());
            disconnect([]() {}, true);
        }
    );
}

void FtpTerminalEngine::disconnect(std::function<void()> onDisconnect, bool byLossOfConnection)
{
    if (impl_->wasDisposed)
        return onDisconnect();

    impl_->wasDisposed = true;
    if (byLossOfConnection)
    {
        Log::info("FTP session '{}' lost its connection", impl_->sessionId.value());
        onDisconnect();
        if (impl_->settings.onConnectionLoss)
            impl_->settings.onConnectionLoss();
        return;
    }
    if (!impl_->sessionId.isValid())
        return onDisconnect();

    Log::info("Disconnecting FTP session '{}'", impl_->sessionId.value());
    Nui::val parameters = Nui::val::object();
    parameters.set("sessionId", impl_->sessionId.value());
    Nui::RpcClient::callWithBackChannel(
        "SessionManager::disconnect",
        [onDisconnect = std::move(onDisconnect)](Nui::val reply)
        {
            if (reply.hasOwnProperty("error"))
                Log::warn("Disconnecting the FTP session failed: {}", reply["error"].as<std::string>());
            onDisconnect();
        },
        parameters
    );
}

void FtpTerminalEngine::dispose(std::function<void()> onDisposeComplete)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("FtpTerminalEngine, dispose already in progress");
        return;
    }
    Log::info("Disposing FtpTerminalEngine");
    impl_->blockedByDestruction = true;
    // The backend closes the channels with the session.
    disconnect(std::move(onDisposeComplete), false);
}

void FtpTerminalEngine::createChannel(
    ChannelCreationOptions const& /*options*/,
    std::function<void(std::string const&)> /*handler*/,
    std::function<void(std::string const&)> /*errorHandler*/,
    std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
    std::function<void(Ids::ChannelId const&)> /*onChannelLoss*/
)
{
    Log::error("An FTP session has no terminals");
    onCreated(std::nullopt, "The session has no terminals");
}

void FtpTerminalEngine::createFileChannel(
    std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated
)
{
    if (impl_->blockedByDestruction)
    {
        Log::error("Blocked by destruction");
        return onCreated(std::nullopt, "Blocked by destruction");
    }

    Nui::val parameters = Nui::val::object();
    parameters.set("fileMode", true);

    Log::info("Creating a file system channel for FTP session '{}'", impl_->sessionId.value());
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::create", impl_->sessionId.value()),
        lifetime_.guarded(
            [this, onCreated = std::move(onCreated)](Nui::val reply)
            {
                if (!reply.hasOwnProperty("id"))
                {
                    const auto error = replyError(reply, "The channel creation returned no channel");
                    Log::error("Failed to create the file system channel: {}", error);
                    onCreated(std::nullopt, error);
                    return;
                }
                const auto channelId = Ids::makeChannelId(reply["id"].as<std::string>());
                impl_->fileChannels.insert(channelId);
                onCreated(channelId, "Channel created");
            }
        ),
        parameters
    );
}

void FtpTerminalEngine::closeFileChannel(Ids::ChannelId const& channelId, std::function<void()> onClosed)
{
    if (impl_->fileChannels.erase(channelId) == 0u)
    {
        Log::error("Failed to close channel (could not find it): {}", channelId.value());
        return onClosed();
    }

    Log::info("FtpTerminalEngine: Closing channel: {}", channelId.value());
    Nui::RpcClient::callWithBackChannel(
        fmt::format("Session::{}::Channel::close", impl_->sessionId.value()),
        [onClosed = std::move(onClosed)](Nui::val)
        {
            onClosed();
        },
        channelId.value()
    );
}

void FtpTerminalEngine::closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose)
{
    closeFileChannel(channelId, std::move(onClose));
}

ChannelInterface* FtpTerminalEngine::channel(Ids::ChannelId const& /*channelId*/)
{
    return nullptr;
}

Ids::SessionId FtpTerminalEngine::sessionId() const
{
    return impl_->sessionId;
}

void FtpTerminalEngine::setOnConnectionLoss(std::function<void()> onConnectionLoss)
{
    impl_->settings.onConnectionLoss = std::move(onConnectionLoss);
}
