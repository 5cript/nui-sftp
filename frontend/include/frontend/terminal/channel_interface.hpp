#pragma once

#include <ids/ids.hpp>

#include <functional>
#include <string>

class ChannelInterface
{
  public:
    virtual void open(
        std::function<void(std::string const&)> onStdout,
        std::function<void(std::string const&)> onStderr,
        std::function<void(Ids::ChannelId const&)> onExit,
        bool fileMode
    ) = 0;
    virtual void write(std::string const& data) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void dispose(std::function<void()> onExit) = 0;
    virtual Ids::ChannelId channelId() const = 0;
    virtual ~ChannelInterface() = default;
};