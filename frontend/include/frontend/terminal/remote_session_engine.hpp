#pragma once

#include <ids/ids.hpp>

#include <functional>
#include <optional>
#include <string>

/**
 * @brief What an engine with a backend session offers besides its terminals: the session, its file system channels
 *        and the loss of its connection. FileEngine works through it for every protocol.
 */
class RemoteSessionEngine
{
  public:
    /**
     * @brief The backend session, valid once the engine opened.
     */
    virtual Ids::SessionId sessionId() const = 0;

    /**
     * @brief Opens a file system channel on the session.
     *
     * @param onCreated Receives the id of the channel, or std::nullopt and the reason on failure.
     */
    virtual void
    createFileChannel(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated) = 0;

    /**
     * @brief Closes a channel that createFileChannel opened.
     */
    virtual void closeFileChannel(Ids::ChannelId const& channelId, std::function<void()> onClosed) = 0;

    /**
     * @brief Replaces what runs when the connection is lost. A ProtoSession opens the engine without one, the Session
     *        that adopts it sets its own so the lost-connection overlay reacts to a later loss.
     */
    virtual void setOnConnectionLoss(std::function<void()> onConnectionLoss) = 0;

    virtual ~RemoteSessionEngine() = default;
};
