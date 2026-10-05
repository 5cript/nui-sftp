#include <log/log.hpp>

#ifdef __EMSCRIPTEN__
#    include <algorithm>
#    include <utility>
#    include <vector>
#endif

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
    namespace
    {
        struct InstalledHook
        {
            std::uint64_t id;
            Log::Level minimumLevel;
            Hook hook;
        };

        std::vector<InstalledHook>& installedHooks()
        {
            static std::vector<InstalledHook> hooks{};
            return hooks;
        }

        std::uint64_t nextHookId = 1;
        int suppressionDepth = 0;
        bool runningHooks = false;

        void removeHook(std::uint64_t id)
        {
            std::erase_if(installedHooks(), [id](InstalledHook const& installed) {
                return installed.id == id;
            });
        }
    }

    HookRegistration::HookRegistration(std::uint64_t id)
        : id_{id}
    {}

    HookRegistration::~HookRegistration()
    {
        if (id_ != 0)
            removeHook(id_);
    }

    HookRegistration::HookRegistration(HookRegistration&& other) noexcept
        : id_{std::exchange(other.id_, 0)}
    {}

    HookRegistration& HookRegistration::operator=(HookRegistration&& other) noexcept
    {
        if (this != &other)
        {
            if (id_ != 0)
                removeHook(id_);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }

    HookRegistration addHook(Log::Level minimumLevel, Hook hook)
    {
        const auto id = nextHookId++;
        installedHooks().push_back(InstalledHook{.id = id, .minimumLevel = minimumLevel, .hook = std::move(hook)});
        return HookRegistration{id};
    }

    HooksSuppressed::HooksSuppressed()
    {
        ++suppressionDepth;
    }

    HooksSuppressed::~HooksSuppressed()
    {
        --suppressionDepth;
    }

    namespace Detail
    {
        bool hooksWant(Log::Level level)
        {
            if (level == Log::Level::Off || suppressionDepth > 0 || runningHooks)
                return false;
            return std::ranges::any_of(installedHooks(), [level](InstalledHook const& installed) {
                return level >= installed.minimumLevel;
            });
        }

        void runHooks(Log::Level level, std::string const& message)
        {
            if (!hooksWant(level))
                return;

            struct Running
            {
                Running()
                {
                    runningHooks = true;
                }
                ~Running()
                {
                    runningHooks = false;
                }
            } running{};

            // A hook may install or remove hooks, so iterate over a copy.
            const auto hooks = installedHooks();
            for (auto const& installed : hooks)
            {
                if (level >= installed.minimumLevel)
                    installed.hook(level, message);
            }
        }
    }

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