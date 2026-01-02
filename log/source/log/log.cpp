#include <log/log.hpp>

namespace Log
{
    namespace Detail
    {
#ifdef __EMSCRIPTEN__
        std::shared_ptr<Logger> logger{};
#else
        Logger logger{};
#endif
    }

#ifndef __EMSCRIPTEN__
    void setupBackendRpcHub(Nui::RpcHub* hub)
    {
        Detail::logger.setup(hub);
    }
#else
    void setupFrontendLogger(
        std::function<void(std::chrono::system_clock::time_point const&, Log::Level, std::string const&)> onLog,
        std::function<void(Log::Level)> onLogLevel
    )
    {
        Log::Detail::logger = std::make_shared<Log::Logger>(std::move(onLog), std::move(onLogLevel));

        const auto callable = Nui::RpcClient::getRemoteCallable("loggerReady");
        if (callable)
            callable();
        else
            Nui::WebApi::Console::error("loggerReady not callable!");
    }
#endif
}