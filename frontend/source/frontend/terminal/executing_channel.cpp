#include <frontend/terminal/executing_channel.hpp>
#include <log/log.hpp>
#include <nui/rpc.hpp>

ExecutingChannel::ExecutingChannel(
    Ids::ChannelId channelId,
    Nui::RpcClient::AutoUnregister stdoutReceiver,
    Nui::RpcClient::AutoUnregister stderrReceiver,
    Nui::RpcClient::AutoUnregister exitReceiver
)
    : channelId_{std::move(channelId)}
    , stdoutReceiver_{std::move(stdoutReceiver)}
    , stderrReceiver_{std::move(stderrReceiver)}
    , exitReceiver_{std::move(exitReceiver)}
{}

void ExecutingChannel::open(
    std::function<void(std::string const&)>,
    std::function<void(std::string const&)>,
    std::function<void(Ids::ChannelId const&)>,
    bool
)
{
    // RPC receivers were registered in ExecutingTerminalEngine::createChannel before
    // ProcessStore::spawn was called, so all output is already routed.
}

void ExecutingChannel::write(std::string const& data)
{
    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::write",
        [](Nui::val) {},
        channelId_.value(),
        Nui::val::global("btoa")(data).as<std::string>()
    );
}

void ExecutingChannel::resize(int cols, int rows)
{
    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::ptyResize",
        [](Nui::val) {},
        channelId_.value(),
        cols,
        rows
    );
}

void ExecutingChannel::dispose(std::function<void()> onExit)
{
    // Unregister receivers before telling the backend to exit so no stale
    // callbacks fire after the process is gone.
    stdoutReceiver_.reset();
    stderrReceiver_.reset();
    exitReceiver_.reset();

    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::exit",
        [onExit = std::move(onExit)](Nui::val)
        {
            onExit();
        },
        channelId_.value()
    );
}