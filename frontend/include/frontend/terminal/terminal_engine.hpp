#pragma once

#include <ids/ids.hpp>
#include <frontend/terminal/channel_interface.hpp>
#include <frontend/terminal/channel_creation_options.hpp>
#include <frontend/terminal/remote_session_engine.hpp>
#include <log/toast.hpp>

#include <string>
#include <functional>

/**
 * @brief How opening an engine ended.
 */
enum class EngineOpenResult
{
    Opened,
    Failed,
    /**
     * @brief The login got no password, the user cancelled its prompt. Trying again on its own would only ask again.
     */
    LoginCancelled
};

class TerminalEngine
{
  public:
    /**
     * @brief Opens the engine, @p onOpen hears how it ended and why it failed.
     * @param failureToast Whether the errors logged when it fails are shown as toasts.
     */
    virtual void open(std::function<void(EngineOpenResult, std::string const&)> onOpen, Log::Toast failureToast) = 0;
    virtual std::string engineName() const = 0;
    virtual void dispose(std::function<void()> onDisposeComplete) = 0;

    /**
     * @brief Spawns a new channel. Engine-specific per-call options are passed
     *        via a polymorphic ChannelCreationOptions; the engine downcasts to
     *        the concrete type it expects. Engines with no per-call options
     *        (e.g. SSH) simply ignore @p options.
     */
    virtual void createChannel(
        ChannelCreationOptions const& options,
        std::function<void(std::string const&)> handler,
        std::function<void(std::string const&)> errorHandler,
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
        std::function<void(Ids::ChannelId const&)> onChannelLoss
    ) = 0;
    virtual void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose = []() {}) = 0;
    virtual ChannelInterface* channel(Ids::ChannelId const& channelId) = 0;

    /**
     * @brief The backend session behind this engine, nothing for an engine without one.
     */
    virtual RemoteSessionEngine* remoteSession()
    {
        return nullptr;
    }

    virtual ~TerminalEngine() = default;
};
