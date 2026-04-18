#pragma once

#include <frontend/terminal/channel_interface.hpp>
#include <frontend/session_snapshot.hpp>
#include <ids/ids.hpp>
#include <nui/rpc.hpp>
#include <nui/frontend/api/timer.hpp>

#include <functional>
#include <memory>
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
     * @brief Primary ("clean construction") constructor.  Used by
     *        ExecutingTerminalEngine::createChannel immediately after
     *        ProcessStore::spawn returns.  The three receivers were registered
     *        against freshly-generated receptacle names in the engine.
     * @param channelId        The process UUID returned by ProcessStore::spawn.
     * @param stdoutReceiver   Already-registered receiver for stdout data.
     * @param stderrReceiver   Already-registered receiver for stderr data.
     * @param exitReceiver     Already-registered receiver for process exit.
     * @param onProcessChange  Called with the channel id and current foreground process cmdline after writes.
     */
    ExecutingChannel(
        Ids::ChannelId channelId,
        Nui::RpcClient::AutoUnregister stdoutReceiver,
        Nui::RpcClient::AutoUnregister stderrReceiver,
        Nui::RpcClient::AutoUnregister exitReceiver,
        std::function<void(Ids::ChannelId const&, std::string const&)> onProcessChange
    );

    /**
     * @brief Adoption constructor.  Used by ExecutingTerminalEngine::adoptChannel
     *        when taking over a process from a different engine instance during
     *        a seamless reconnect.  The receivers must already be registered at
     *        @p adoption.stdoutReceptacle / stderrReceptacle / "execTerminalExit_"
     *        + adoption.processId so the backend, which is oblivious to the
     *        engine swap, keeps hitting live handlers at the same receptacle
     *        names.  Semantically identical to the primary constructor — the
     *        separate overload exists so the call site reads "adoption".
     */
    ExecutingChannel(
        LocalShellAdoption const& adoption,
        Nui::RpcClient::AutoUnregister stdoutReceiver,
        Nui::RpcClient::AutoUnregister stderrReceiver,
        Nui::RpcClient::AutoUnregister exitReceiver,
        std::function<void(Ids::ChannelId const&, std::string const&)> onProcessChange
    );
    ~ExecutingChannel() override;
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
    void updatePtyProcs();

    /**
     * @brief Shared by every async callback the channel spawns (timer, RPC
     *        response). The destructor / dispose() flip @c alive to false so
     *        callbacks that fire after destruction short-circuit instead of
     *        touching freed memory. @c queryInFlight is the re-entry guard
     *        that replaces the old Nui::TimerHandle::hasActiveTimer() check,
     *        which kept returning true after the timer had already fired —
     *        blocking every subsequent process-info query.
     */
    struct AsyncState
    {
        bool alive{true};
        bool queryInFlight{false};
    };

    Ids::ChannelId channelId_;
    Nui::RpcClient::AutoUnregister stdoutReceiver_;
    Nui::RpcClient::AutoUnregister stderrReceiver_;
    Nui::RpcClient::AutoUnregister exitReceiver_;
    std::function<void(Ids::ChannelId const&, std::string const&)> onProcessChange_;
    Nui::TimerHandle procInfoTimer_;
    std::shared_ptr<AsyncState> asyncState_;
};