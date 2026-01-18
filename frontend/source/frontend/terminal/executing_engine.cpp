#include <exception>
#include <frontend/terminal/executing_engine.hpp>
#include <frontend/nlohmann_compat.hpp>
#include <log/log.hpp>
#include <nui/frontend/api/timer.hpp>

#include <nui/rpc.hpp>

using namespace std::string_literals;

struct ExecutingTerminalEngine::Implementation
{
    ExecutingTerminalEngine::Settings settings;
    std::string id;

    Nui::RpcClient::AutoUnregister stdoutReceiver;
    Nui::RpcClient::AutoUnregister stderrReceiver;

    std::string processId;

    std::function<void(std::string const&)> stdoutHandler;
    std::function<void(std::string const&)> stderrHandler;

    Nui::TimerHandle procInfoTimer;

    Implementation(ExecutingTerminalEngine::Settings&& settings)
        : settings{std::move(settings)}
        , id{Nui::val::global("generateId")().as<std::string>()}
        , stdoutReceiver{}
        , stderrReceiver{}
        , processId{}
        , stdoutHandler{}
        , stderrHandler{}
        , procInfoTimer{}
    {}
};

ExecutingTerminalEngine::ExecutingTerminalEngine(Settings settings)
    : impl_{std::make_unique<Implementation>(std::move(settings))}
{}
ExecutingTerminalEngine::~ExecutingTerminalEngine()
{
    if (!moveDetector_.wasMoved())
    {
        dispose([]() {});
    }
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(ExecutingTerminalEngine);

void ExecutingTerminalEngine::open(std::function<void(bool, std::string const&)> onOpen)
{
    impl_->stdoutReceiver = Nui::RpcClient::autoRegisterFunction(
        "execTerminalStdout_" + impl_->id,
        [this](Nui::val val)
        {
            if (val.hasOwnProperty("data"))
            {
                const std::string data = Nui::val::global("atob")(val["data"]).as<std::string>();
                if (impl_->stdoutHandler)
                    impl_->stdoutHandler(data);
            }
            else
                Log::error("execTerminalStdout_" + impl_->id + " received an empty message");
        }
    );

    impl_->stderrReceiver = Nui::RpcClient::autoRegisterFunction(
        "execTerminalStderr_" + impl_->id,
        [this](Nui::val val)
        {
            if (val.hasOwnProperty("data"))
            {
                const std::string data = Nui::val::global("atob")(val["data"]).as<std::string>();
                if (impl_->stderrHandler)
                    impl_->stderrHandler(data);
            }
            else
                Log::error("execTerminalStderr_" + impl_->id + " received an empty message");
        }
    );

    Nui::val obj = Nui::val::object();

    obj.set("command", impl_->settings.engineOptions.command);
    if (impl_->settings.engineOptions.arguments)
    {
        Nui::val args = Nui::val::array();
        for (auto const& arg : *impl_->settings.engineOptions.arguments)
        {
            args.call<void>("push", arg);
        }
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
        {
            env.set(key.c_str(), value);
        }
        obj.set("environment", env);
    }
    else
    {
        obj.set("environment", Nui::val::object());
    }

    obj.set("defaultExitWaitTimeout", impl_->settings.engineOptions.exitTimeoutSeconds);
    obj.set("cleanEnvironment", impl_->settings.engineOptions.cleanEnvironment);
    obj.set("isPty", impl_->settings.engineOptions.isPty);

    obj.set("stdout", "execTerminalStdout_" + impl_->id);
    obj.set("stderr", "execTerminalStderr_" + impl_->id);
    try
    {
        obj.set("termios", asVal(impl_->settings.termios));
    }
    catch (std::exception const& exc)
    {
        Log::error("Failed to serialize termios: {}", exc.what());
    }

    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::spawn",
        [this, onOpen = std::move(onOpen)](Nui::val val)
        {
            if (!val.hasOwnProperty("id"))
            {
                Log::error("ProcessStore::spawn callback did not return an id");
                if (val.hasOwnProperty("error"))
                    Log::error(val["error"].as<std::string>());
                return onOpen(false, val["error"].as<std::string>());
            }
            // TODO: Use typed id
            std::string id = val["id"].as<std::string>();
            impl_->processId = id;

            onOpen(true, id);
            updatePtyProcs();
        },
        obj
    );
}

void ExecutingTerminalEngine::dispose(std::function<void()> onDisposeComplete)
{
    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::exit",
        [onDisposeComplete = std::move(onDisposeComplete)](Nui::val)
        {
            // TODO: handle error
            onDisposeComplete();
        },
        impl_->processId
    );

    impl_->stdoutReceiver.reset();
    impl_->stderrReceiver.reset();
}

void ExecutingTerminalEngine::resize(int cols, int rows)
{
    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::ptyResize",
        [](Nui::val)
        {
            // TODO: handle error
        },
        impl_->processId,
        cols,
        rows
    );
}

void ExecutingTerminalEngine::updatePtyProcs()
{
    Log::info("updatePtyProcs");
    if (!impl_->procInfoTimer.hasActiveTimer())
    {
        Nui::setTimeout(
            500,
            [this]()
            {
                Nui::RpcClient::callWithBackChannel(
                    "ProcessStore::ptyProcesses",
                    [this](Nui::val val)
                    {
                        if (val.hasOwnProperty("latest"))
                        {
                            Log::info("onProcessChange: {}", Nui::JSON::stringify(val));
                            if (impl_->settings.onProcessChange)
                                impl_->settings.onProcessChange(val["latest"]["cmdline"].as<std::string>());
                        }
                        else
                        {
                            Log::warn("ptyProcesses did not return latest: {}", Nui::JSON::stringify(val));
                        }
                    },
                    impl_->processId
                );
            },
            [this](Nui::TimerHandle&& handle)
            {
                impl_->procInfoTimer = std::move(handle);
            }
        );
    }
}

std::string ExecutingTerminalEngine::id() const
{
    return impl_->processId;
}

void ExecutingTerminalEngine::write(std::string const& data)
{
    if (!data.empty() && (data.back() == '\r' || data.back() == '\n'))
        updatePtyProcs();

    Nui::RpcClient::callWithBackChannel(
        "ProcessStore::write", [](Nui::val) {}, impl_->processId, Nui::val::global("btoa")(data).as<std::string>()
    );
}

void ExecutingTerminalEngine::setStdoutHandler(std::function<void(std::string const&)> handler)
{
    impl_->stdoutHandler = std::move(handler);
}
void ExecutingTerminalEngine::setStderrHandler(std::function<void(std::string const&)> handler)
{
    impl_->stderrHandler = std::move(handler);
}