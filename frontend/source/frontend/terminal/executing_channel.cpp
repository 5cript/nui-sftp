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
    , asyncState_{std::make_shared<AsyncState>()}
{
    updatePtyProcs();
}

ExecutingChannel::~ExecutingChannel()
{
    // Moved-from instances have a null asyncState_; the live instance still
    // owns the shared state and flips `alive` so pending timer / RPC lambdas
    // short-circuit when they fire after destruction.
    if (asyncState_)
        asyncState_->alive = false;
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
    if (!asyncState_ || asyncState_->queryInFlight)
        return;
    asyncState_->queryInFlight = true;

    // Capture by value so the lambdas are decoupled from `this`. If the
    // channel is destroyed before the timer fires or before the RPC response
    // arrives, the alive-flag in asyncState protects us.
    auto state = asyncState_;
    auto channelId = channelId_;
    auto onProcessChange = onProcessChange_;

    Nui::setTimeout(
        500,
        [state, channelId, onProcessChange]()
        {
            if (!state->alive)
            {
                state->queryInFlight = false;
                return;
            }
            Log::info("ExecutingChannel: querying ptyProcesses for channelId={}", channelId.value());
            Nui::RpcClient::callWithBackChannel(
                "ProcessStore::ptyProcesses",
                [state, channelId, onProcessChange](Nui::val val)
                {
                    state->queryInFlight = false;
                    if (!state->alive)
                        return;
                    if (val.hasOwnProperty("latest"))
                    {
                        if (onProcessChange)
                            onProcessChange(
                                channelId,
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
                channelId.value()
            );
        },
        [this, state](Nui::TimerHandle&& handle)
        {
            // Fires synchronously-ish from setTimeout to deliver the handle.
            // Guard `this` behind the alive flag to cover the unlikely case
            // where destruction happens between schedule and delivery.
            if (!state->alive)
                return;
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

    // Mark async callbacks inert before anything else — the timer or an
    // in-flight ptyProcesses RPC response must not reach `this` once the
    // channel has been disposed.
    if (asyncState_)
        asyncState_->alive = false;
    procInfoTimer_ = {};

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