#pragma once

#include <ids/ids.hpp>
#include <frontend/terminal/channel_interface.hpp>

#include <string>
#include <functional>

class TerminalEngine
{
  public:
    virtual void open(std::function<void(bool, std::string const&)> onOpen) = 0;
    virtual std::string engineName() const = 0;
    virtual void dispose(std::function<void()> onDisposeComplete) = 0;
    virtual ~TerminalEngine() = default;
};

class MultiChannelTerminalEngine : public TerminalEngine
{
  public:
    virtual void createChannel(
        std::function<void(std::string const&)> handler,
        std::function<void(std::string const&)> errorHandler,
        std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated
    ) = 0;
    virtual void
    createSftpChannel(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated) = 0;
    virtual void closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose = []() {}) = 0;
    virtual ChannelInterface* channel(Ids::ChannelId const& channelId) = 0;
};

class SingleChannelTerminalEngine : public TerminalEngine
{
  public:
    virtual void write(std::string const& data) = 0;
    virtual void resize(int cols, int rows) = 0;

    virtual void setStdoutHandler(std::function<void(std::string const&)> handler) = 0;
    virtual void setStderrHandler(std::function<void(std::string const&)> handler) = 0;
};