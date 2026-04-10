#include <frontend/terminal/executing_engine.hpp>
#include <frontend/terminal/executing_channel.hpp>
#include <frontend/nlohmann_compat.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>

#include <unordered_map>

using namespace std::string_literals;

struct ExecutingTerminalEngine::Implementation
{
    ExecutingTerminalEngine::Settings settings;
    std::string engineId;

    std::unordered_map<Ids::ChannelId, ExecutingChannel, Ids::IdHash> channels;

    explicit Implementation(ExecutingTerminalEngine::Settings&& settings)
        : settings{std::move(settings)}
        , engineId{Nui::val::global("generateId")().as<std::string>()}
        , channels{}
    {}
};

ExecutingTerminalEngine::ExecutingTerminalEngine(Settings settings)
    : impl_{std::make_unique<Implementation>(std::move(settings))}
{}
ExecutingTerminalEngine::~ExecutingTerminalEngine()
{
    if (!moveDetector_.wasMoved())
        dispose([]() {});
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(ExecutingTerminalEngine);

void ExecutingTerminalEngine::open(std::function<void(bool, std::string const&)> onOpen)
{
    // Local processes need no prior connection — signal success immediately.
    onOpen(true, impl_->engineId);
}

void ExecutingTerminalEngine::dispose(std::function<void()> onDisposeComplete)
{
    // Exit every active process. Since each dispose() is async we fire them all and
    // call onDisposeComplete once the last one finishes (or immediately if empty).
    if (impl_->channels.empty())
    {
        onDisposeComplete();
        return;
    }

    auto remaining = std::make_shared<std::size_t>(impl_->channels.size());
    auto complete = std::make_shared<std::function<void()>>(std::move(onDisposeComplete));

    for (auto& [id, chan] : impl_->channels)
    {
        chan.dispose(
            [remaining, complete]()
            {
                if (--(*remaining) == 0)
                    (*complete)();
            }
        );
    }
    impl_->channels.clear();
}

std::string ExecutingTerminalEngine::id() const
{
    // Returns the engine-level ID, not a process UUID.
    // SessionArea::processDied matching will therefore never fire for this engine,
    // which is correct — channel exit is handled per-channel via execTerminalExit_.
    return impl_->engineId;
}

void ExecutingTerminalEngine::createChannel(
    std::function<void(std::string const&)> handler,
    std::function<void(std::string const&)> errorHandler,
    std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated,
    std::function<void(Ids::ChannelId const&)> onChannelLoss
)
{
    // Generate a stable local ID for the RPC receptacle names so that stdout/stderr
    // receivers can be registered before ProcessStore::spawn is called, ensuring no
    // output is lost even if the process produces output immediately on start.
    const std::string localId = Nui::val::global("generateId")().as<std::string>();
    const std::string stdoutReceptacle = "execTerminalStdout_" + localId;
    const std::string stderrReceptacle = "execTerminalStderr_" + localId;
    Log::info(
        "ExecutingTerminalEngine::createChannel called, localId={}, stdoutReceptacle={}, stderrReceptacle={}",
        localId,
        stdoutReceptacle,
        stderrReceptacle
    );

    // Register stdout receiver. The handler lambda writes decoded data to xterm.js
    // via the TerminalChannel (FrontendSessionManager wires this up).
    auto stdoutReceiver = Nui::RpcClient::autoRegisterFunction(
        stdoutReceptacle,
        [handler](Nui::val val)
        {
            if (val.hasOwnProperty("data"))
                handler(Nui::val::global("atob")(val["data"]).as<std::string>());
            else
                Log::error("execTerminalStdout received message without data field");
        }
    );

    auto stderrReceiver = Nui::RpcClient::autoRegisterFunction(
        stderrReceptacle,
        [errorHandler](Nui::val val)
        {
            if (val.hasOwnProperty("data"))
                errorHandler(Nui::val::global("atob")(val["data"]).as<std::string>());
            else
                Log::error("execTerminalStderr received message without data field");
        }
    );

    // Build the spawn parameters the same way the old single-channel engine did.
    Nui::val obj = Nui::val::object();
    obj.set("command", impl_->settings.engineOptions.command.generic_string());

    if (impl_->settings.engineOptions.arguments)
    {
        Nui::val args = Nui::val::array();
        for (auto const& arg : *impl_->settings.engineOptions.arguments)
            args.call<void>("push", arg);
        obj.set("arguments", args);
    }
    else
    {
        obj.set("arguments", Nui::val::array());
    }

    if (impl_->settings.engineOptions.environment)
    {
        Nui::val env = Nui::val::object();
        for (auto const& [key, value] : *impl_->settings.engineOptions.environment)
            env.set(key.c_str(), value);
        obj.set("environment", env);
    }
    else
    {
        obj.set("environment", Nui::val::object());
    }

    obj.set("defaultExitWaitTimeout", impl_->settings.engineOptions.exitTimeoutSeconds);
    obj.set("cleanEnvironment", impl_->settings.engineOptions.cleanEnvironment);
    obj.set("isPty", impl_->settings.engineOptions.isPty);
    obj.set("stdout", stdoutReceptacle);
    obj.set("stderr", stderrReceptacle);
    try
    {
        obj.set("termios", asVal(impl_->settings.termios));
    }
    catch (std::exception const& exc)
    {
        Log::error("Failed to serialize termios: {}", exc.what());
    }

    // Move the receivers into shared_ptrs so the spawn callback can move them into
    // the channel even though the callback is a std::function (no move-only captures).
    auto sharedStdout = std::make_shared<Nui::RpcClient::AutoUnregister>(std::move(stdoutReceiver));
    auto sharedStderr = std::make_shared<Nui::RpcClient::AutoUnregister>(std::move(stderrReceiver));

    Log::info("ExecutingTerminalEngine: calling ProcessStore::spawn for localId={}", localId);
    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::spawn",
        [this,
            localId,
            sharedStdout = std::move(sharedStdout),
            sharedStderr = std::move(sharedStderr),
            onCreated = std::move(onCreated),
            onChannelLoss = std::move(onChannelLoss)](Nui::val val)
        {
            Log::info("ExecutingTerminalEngine: ProcessStore::spawn response received for localId={}", localId);
            if (!val.hasOwnProperty("id"))
            {
                Log::error(
                    "ProcessStore::spawn did not return an id: {}",
                    val.hasOwnProperty("error") ? val["error"].as<std::string>() : "(no error field)"
                );
                onCreated(std::nullopt, val.hasOwnProperty("error") ? val["error"].as<std::string>() : "spawn failed");
                return;
            }

            const std::string processId = val["id"].as<std::string>();
            Log::info("ExecutingTerminalEngine: spawn succeeded, processId={}", processId);
            const Ids::ChannelId channelId = Ids::makeChannelId(processId);

            // Register the per-process exit receiver.  The backend sends to
            // "execTerminalExit_<processId>" when the process terminates.
            auto exitReceiver = Nui::RpcClient::autoRegisterFunction(
                "execTerminalExit_" + processId,
                [this, channelId, onChannelLoss](Nui::val)
                {
                    Log::info("ExecutingTerminalEngine: process '{}' exited.", channelId.value());
                    onChannelLoss(channelId);
                    impl_->channels.erase(channelId);
                }
            );
            auto sharedExit = std::make_shared<Nui::RpcClient::AutoUnregister>(std::move(exitReceiver));

            impl_->channels.try_emplace(
                channelId,
                channelId,
                std::move(*sharedStdout),
                std::move(*sharedStderr),
                std::move(*sharedExit),
                impl_->settings.onProcessChange
            );

            Log::info(
                "ExecutingTerminalEngine: channel emplace done, calling onCreated for channelId={}", channelId.value()
            );
            onCreated(channelId, "");
        },
        obj
    );
}

void ExecutingTerminalEngine::createSftpChannel(
    std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> onCreated
)
{
    onCreated(std::nullopt, "SFTP is not supported by the local process engine");
}

void ExecutingTerminalEngine::closeChannel(Ids::ChannelId const& channelId, std::function<void()> onClose)
{
    auto iter = impl_->channels.find(channelId);
    if (iter == impl_->channels.end())
    {
        onClose();
        return;
    }

    iter->second.dispose(
        [this, channelId, onClose = std::move(onClose)]()
        {
            impl_->channels.erase(channelId);
            onClose();
        }
    );
}

ChannelInterface* ExecutingTerminalEngine::channel(Ids::ChannelId const& channelId)
{
    auto iter = impl_->channels.find(channelId);
    return iter != impl_->channels.end() ? &iter->second : nullptr;
}