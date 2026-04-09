#include <log/logger_backend.hpp>
#include <nui/backend/filesystem/special_paths.hpp>
#include <constants/persistence.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/dist_sink.h>

#include <filesystem>
#include <string>

namespace Log
{
    namespace fs = std::filesystem;

    void Logger::setupGlobalSinks(Log::Level level, std::filesystem::path const& directory, bool disableFileLog)
    {
        const auto sinkDir = Nui::resolvePath(directory);
        const auto spdlogLevel = toSpdlogLevel(level);

        auto dist_sink = std::make_shared<spdlog::sinks::dist_sink_st>();

        // ---- Console sink (stderr) ----
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        stderr_sink->set_level(spdlogLevel);
        dist_sink->add_sink(stderr_sink);

        // ---- File sink (optional) ----
        if (!disableFileLog)
        {
            fs::path log_dir =
                fmt::format(fmt::runtime(sinkDir.generic_string()), fmt::arg("appName", Constants::appName));
            std::error_code ec;
            fs::create_directories(log_dir, ec); // best-effort

            fs::path log_file = log_dir / (std::string{Constants::appName} + ".log");

            // 5 MB per file, keep 3 rotated files
            auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file.string(), 5 * 1024 * 1024, 3, true);

            file_sink->set_level(spdlogLevel);
            dist_sink->add_sink(file_sink);
        }

        // ---- Global logger ----
        auto logger = std::make_shared<spdlog::logger>(std::string{Constants::appName}, dist_sink);

        logger->set_level(spdlogLevel);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn);
    }

    void Logger::setup(Nui::RpcHub* hub)
    {
        std::scoped_lock lock{guard_};
        if (hub == nullptr)
        {
            rpcHub_ = nullptr;
            rpcHubPrelim_ = nullptr;
            return;
        }

        rpcHubPrelim_ = hub;

        rpcHubPrelim_->registerFunction(
            "loggerReady",
            [this]()
            {
                std::scoped_lock lock{guard_};
                rpcHub_ = rpcHubPrelim_;
                rpcHubPrelim_ = nullptr;

                if (!stash_.empty())
                {
                    for (auto& s : stash_)
                        logImpl(s.first, s.second);
                    stash_.clear();
                }
                rpcHub_->callRemote("setLogLevel", static_cast<int>(levelStashed_));
            }
        );
        rpcHubPrelim_->registerFunction(
            "log",
            [](int integralLevel, std::string const& message)
            {
                spdlog::log(toSpdlogLevel(static_cast<Log::Level>(integralLevel)), message);
            }
        );
        rpcHubPrelim_->registerFunction(
            "setLogLevel",
            [](int integralLevel)
            {
                spdlog::set_level(toSpdlogLevel(static_cast<Log::Level>(integralLevel)));
            }
        );
    }

    void Logger::setLevel(Log::Level level)
    {
        std::scoped_lock lock{guard_};
        if (rpcHub_ != nullptr)
            rpcHub_->callRemote("setLogLevel", static_cast<int>(level));
        else
        {
            levelStashed_ = level;
        }
        spdlog::set_level(toSpdlogLevel(level));
    }

    Log::Level Logger::level() const
    {
        return fromSpdlogLevel(spdlog::get_level());
    }

    void Logger::detach()
    {
        std::scoped_lock lock{guard_};
        rpcHub_ = nullptr;
        rpcHubPrelim_ = nullptr;
        stash_.clear();
    }

    void Logger::logImpl(Log::Level level, std::string const& msg)
    {
        decltype(rpcHub_) hub;
        {
            std::scoped_lock lock{guard_};
            hub = rpcHub_;
        }
        if (hub != nullptr)
        {
            try
            {
                hub->callRemote("log", static_cast<int>(level), msg);
            }
            catch (const std::exception& e)
            {
                std::string what = e.what();
                spdlog::log(
                    toSpdlogLevel(level), "Failed to send log message to frontend: {}. Original message: {}", what, msg
                );
            }
        }
        else
        {
            std::scoped_lock lock{guard_};
            // Do not accumulate logs in tests:
            if (level != Level::Off)
            {
                stash_.emplace_back(level, msg);
            }
        }

        spdlog::log(toSpdlogLevel(level), msg);
    }
}