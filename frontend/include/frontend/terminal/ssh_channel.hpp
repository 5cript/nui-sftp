#pragma once

#include <frontend/terminal/channel_interface.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/val.hpp>
#include <nui/utility/move_detector.hpp>
#include <nui/rpc.hpp>

#include <functional>
#include <string>

class SshChannel : public ChannelInterface
{
  public:
    SshChannel(Ids::SessionId sessionId, Ids::ChannelId channelId);
    ~SshChannel();
    SshChannel(SshChannel const&) = delete;
    SshChannel& operator=(SshChannel const&) = delete;
    SshChannel(SshChannel&&) = default;
    SshChannel& operator=(SshChannel&&) = default;

    void open(
        std::function<void(std::string const&)> onStdout,
        std::function<void(std::string const&)> onStderr,
        std::function<void(Ids::ChannelId const&)> onExit,
        bool fileMode
    ) override;
    void write(std::string const& data) override;
    void resize(int cols, int rows) override;
    void dispose(std::function<void()> onExit) override;

    Ids::ChannelId channelId() const override
    {
        return sshChannelId_;
    }
    Ids::SessionId sessionId() const
    {
        return sshSessionId_;
    }

  private:
    Nui::MoveDetector moveDetector_;
    Nui::RpcClient::AutoUnregister stdoutReceiver_;
    Nui::RpcClient::AutoUnregister stderrReceiver_;
    Nui::RpcClient::AutoUnregister onExitReceiver_;
    Ids::ChannelId sshChannelId_;
    Ids::SessionId sshSessionId_;

    std::function<void(std::string const&)> stdoutHandler_;
    std::function<void(std::string const&)> stderrHandler_;
    std::function<void()> onExitHandler_;
};