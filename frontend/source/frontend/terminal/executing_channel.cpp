#include <frontend/terminal/executing_channel.hpp>
#include <log/log.hpp>
#include <nui/rpc.hpp>

ExecutingChannel::ExecutingChannel(
    Ids::ChannelId channelId,
    Nui::RpcClient::AutoUnregister stdoutReceiver,
    Nui::RpcClient::AutoUnregister stderrReceiver,
    Nui::RpcClient::AutoUnregister exitReceiver,
    std::function<void(Ids::ChannelId const&, std::string const&)> onProcessChange
)
    : channelId_{std::move(channelId)}
    , stdoutReceiver_{std::move(stdoutReceiver)}
    , stderrReceiver_{std::move(stderrReceiver)}
    , exitReceiver_{std::move(exitReceiver)}
    , onProcessChange_{std::move(onProcessChange)}
    , procInfoTimer_{}
{
    updatePtyProcs();
}

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
    if (!data.empty() && (data.back() == '\r' || data.back() == '\n'))
        updatePtyProcs();

    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::write", [](Nui::val) {}, channelId_.value(), Nui::val::global("btoa")(data).as<std::string>()
    );
}

void ExecutingChannel::updatePtyProcs()
{
    if (procInfoTimer_.hasActiveTimer())
        return;

    Nui::setTimeout(
        500,
        [this]()
        {
            Log::info("ExecutingChannel: querying ptyProcesses for channelId={}", channelId_.value());
            Nui::RpcClient::callWithBackChannel(
                "ProcessStore::ptyProcesses",
                [this](Nui::val val)
                {
                    if (val.hasOwnProperty("latest"))
                    {
                        if (onProcessChange_)
                            onProcessChange_(
                                channelId_,
                                fmt::format(
                                    "{} ({})",
                                    val["latest"]["cmdline"].as<std::string>(),
                                    val["latest"]["pid"].as<int>()
                                )
                            );
                    }
                    else
                    {
                        Log::warn("ptyProcesses did not return latest: {}", Nui::JSON::stringify(val));
                    }
                },
                channelId_.value()
            );
        },
        [this](Nui::TimerHandle&& handle)
        {
            procInfoTimer_ = std::move(handle);
        }
    );
}

void ExecutingChannel::resize(int cols, int rows)
{
    Nui::RpcClient::callWithBackChannel("ProcessStore::ptyResize", [](Nui::val) {}, channelId_.value(), cols, rows);
}

void ExecutingChannel::dispose(std::function<void()> onExit)
{
    Log::info("ExecutingChannel: disposing channelId={}", channelId_.value());

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