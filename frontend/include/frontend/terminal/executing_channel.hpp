#pragma once

#include <frontend/terminal/channel_interface.hpp>
#include <ids/ids.hpp>
#include <nui/rpc.hpp>

#include <functional>
#include <string>

/**
 * @brief Represents a single local process as a ChannelInterface.
 *
 * One ExecutingChannel owns exactly one backend process (identified by its UUID).
 * The stdout/stderr/exit RPC receivers are registered by ExecutingTerminalEngine::createChannel
 * before spawn so no output is missed. open() is therefore a no-op.
 */
class ExecutingChannel : public ChannelInterface
{
  public:
    /**
     * @param channelId       The process UUID returned by ProcessStore::spawn.
     * @param stdoutReceiver  Already-registered receiver for stdout data.
     * @param stderrReceiver  Already-registered receiver for stderr data.
     * @param exitReceiver    Already-registered receiver for process exit.
     */
    ExecutingChannel(
        Ids::ChannelId channelId,
        Nui::RpcClient::AutoUnregister stdoutReceiver,
        Nui::RpcClient::AutoUnregister stderrReceiver,
        Nui::RpcClient::AutoUnregister exitReceiver
    );
    ~ExecutingChannel() override = default;
    ExecutingChannel(ExecutingChannel&&) = default;
    ExecutingChannel& operator=(ExecutingChannel&&) = default;
    ExecutingChannel(ExecutingChannel const&) = delete;
    ExecutingChannel& operator=(ExecutingChannel const&) = delete;

    /** @brief No-op: receivers were already registered before ProcessStore::spawn. */
    void open(
        std::function<void(std::string const&)> onStdout,
        std::function<void(std::string const&)> onStderr,
        std::function<void(Ids::ChannelId const&)> onExit,
        bool fileMode
    ) override;

    void write(std::string const& data) override;
    void resize(int cols, int rows) override;

    /** @brief Sends ProcessStore::exit for the owned process, then calls onExit. */
    void dispose(std::function<void()> onExit) override;

    Ids::ChannelId channelId() const override
    {
        return channelId_;
    }

  private:
    Ids::ChannelId channelId_;
    Nui::RpcClient::AutoUnregister stdoutReceiver_;
    Nui::RpcClient::AutoUnregister stderrReceiver_;
    Nui::RpcClient::AutoUnregister exitReceiver_;
};