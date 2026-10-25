#pragma once

#include <ids/ids.hpp>
#include <frontend/terminal/channel_interface.hpp>
#include <frontend/terminal/channel_creation_options.hpp>
#include <log/toast.hpp>

#include <string>
#include <functional>

class TerminalEngine
{
  public:
    /**
     * @brief Opens the engine, @p onOpen hears whether it succeeded.
     * @param failureToast Whether the errors logged when it fails are shown as toasts.
     */
    virtual void open(std::function<void(bool, std::string const&)> onOpen, Log::Toast failureToast) = 0;
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
    virtual void
    createSftpChannel(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated) = 0;
    virtual void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose = []() {}) = 0;
    virtual ChannelInterface* channel(Ids::ChannelId const& channelId) = 0;

    virtual ~TerminalEngine() = default;
};
