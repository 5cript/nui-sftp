#pragma once

#include <log/level.hpp>
#include <log/def.hpp>
#include <log/toast.hpp>

#ifdef __EMSCRIPTEN__
#    include <log/logger_frontend.hpp>
#    include <nui/frontend/rpc_client.hpp>
#    include <fmt/format.h>
#    include <fmt/ranges.h>
#else
#    include <log/logger_backend.hpp>
#    include <spdlog/spdlog.h>
#endif

#include <string>
#include <string_view>

namespace Log
{
    namespace Detail
    {
#ifdef __EMSCRIPTEN__
        extern std::shared_ptr<Logger> logger;
#else
        extern Logger logger;
#endif
    }

    /**
     * @brief Log a message.
     *
     * @tparam Args Format template arguments.
     * @param level Log level.
     * @param toast Whether the frontend's log hooks see it, which show errors as toasts.
     * @param fmt Format string.
     * @param args Format string arguments.
     */
    template <typename... Args>
    void log(Log::Level level, Log::Toast toast, std::string_view fmt, Args&&... args)
    {
#ifdef __EMSCRIPTEN__
        const auto callable = Nui::RpcClient::getRemoteCallable("log");
        if (callable)
            callable(static_cast<int>(level), Logger::format(fmt, std::forward<Args>(args)...));

        if (Detail::logger)
            Detail::logger->log(level, toast, fmt, std::forward<Args>(args)...);
#else
        Detail::logger.log(level, toast, fmt, std::forward<Args>(args)...);
#endif
    }

    /**
     * @brief Log a message, shown as a toast when it is an error.
     *
     * @tparam Args Format template arguments.
     * @param level Log level.
     * @param fmt Format string.
     * @param args Format string arguments.
     */
    template <typename... Args>
    void log(Log::Level level, std::string_view fmt, Args&&... args)
    {
        log(level, Log::Toast::Show, fmt, std::forward<Args>(args)...);
    }

#ifndef __EMSCRIPTEN__
    /**
     * @brief Setups the backend rpc hub for logging.
     *
     * @param hub
     */
    void setupBackendRpcHub(Nui::RpcHub* hub);
#else
    void setupFrontendLogger(
        std::function<void(std::chrono::system_clock::time_point const&, Log::Level, std::string const&)> onLog,
        std::function<void(Log::Level)> onLogLevel);
#endif

    /**
     * @brief Set the log level.
     *
     * @param level
     */
    inline void setLevel(Log::Level level)
    {
#ifdef __EMSCRIPTEN__
        if (Detail::logger)
            Detail::logger->setLevel(level);
#else
        Detail::logger.setLevel(level);
#endif
    }

    /**
     * @brief Get the log level.
     *
     * @return Log::Level
     */
    inline Log::Level level()
    {
#ifdef __EMSCRIPTEN__
        if (Detail::logger)
            return Detail::logger->level();
        return Log::Level::Info;
#else
        return Detail::logger.level();
#endif
    }

    /**
     * @brief Convenience function to log at trace level.
     */
    template <typename... Args>
    inline void trace(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Trace, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at debug level.
     */
    template <typename... Args>
    inline void debug(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Debug, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at info level.
     */
    template <typename... Args>
    inline void info(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Info, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at warn level.
     */
    template <typename... Args>
    inline void warn(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Warning, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at warn level, @p toast decides whether the log hooks see it.
     */
    template <typename... Args>
    inline void warn(Log::Toast toast, std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Warning, toast, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at error level.
     */
    template <typename... Args>
    inline void error(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Error, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at error level, @p toast decides whether the log hooks see it.
     */
    template <typename... Args>
    inline void error(Log::Toast toast, std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Error, toast, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at critical level.
     */
    template <typename... Args>
    inline void critical(std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Critical, std::move(fmt), std::forward<Args>(args)...);
    }

    /**
     * @brief Convenience function to log at critical level, @p toast decides whether the log hooks see it.
     */
    template <typename... Args>
    inline void critical(Log::Toast toast, std::string_view fmt, Args&&... args)
    {
        return log(Log::Level::Critical, toast, std::move(fmt), std::forward<Args>(args)...);
    }
}