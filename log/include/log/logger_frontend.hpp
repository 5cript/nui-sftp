#pragma once

#include <log/level.hpp>
#include <log/def.hpp>

#include <fmt/format.h>
#include <nui/frontend/rpc_client.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/api/json.hpp>
#include <nui/frontend/utility/val_conversion.hpp>

#include <functional>
#include <chrono>
#include <string>
#include <string_view>

namespace Log
{
    namespace Detail
    {
        template <typename T>
        struct AsFormattable
        {
            template <typename U = T>
            static T&& convert(U&& t)
            {
                return std::forward<U>(t);
            }
        };

        template <>
        struct AsFormattable<Nui::val>
        {
            static std::string convert(Nui::val const& t)
            {
                return Nui::JSON::stringify(t);
            }
        };
    }

    class Logger
    {
      public:
        Logger(
            std::function<void(std::chrono::system_clock::time_point const&, Log::Level, std::string const&)> onLog,
            std::function<void(Log::Level)> onLogLevel,
            bool logOnConsole = true
        )
            : onLog_{std::move(onLog)}
            , logOnConsole_{logOnConsole}
            , autoUnregisterOnLog_{Nui::RpcClient::autoRegisterFunction(
                  "log",
                  [this](int integralLevel, std::string const& message)
                  {
                      using namespace std::string_literals;
                      log(static_cast<Log::Level>(integralLevel), "[MAIN] "s + message);
                  }
              )}
            , autoUnregisterSetLevel_{Nui::RpcClient::autoRegisterFunction(
                  "setLogLevel",
                  [this, onLogLevel = std::move(onLogLevel)](int integralLevel)
                  {
                      logLevel_ = static_cast<Log::Level>(integralLevel);
                      onLogLevel(logLevel_);
                  }
              )}
            , logLevel_{Log::Level::Info}
        {}

      public:
        void setLevel(Log::Level level)
        {
            logLevel_ = level;

            const auto callable = Nui::RpcClient::getRemoteCallable("setLogLevel");
            if (callable)
                callable(static_cast<int>(level));
        }

        Log::Level level() const
        {
            return logLevel_;
        }

        template <typename... Args>
        void log(Log::Level level, std::string const& message)
        {
            if (level < logLevel_)
                return;

            onLog_(std::chrono::system_clock::now(), level, message);

            if (logOnConsole_)
            {
                switch (level)
                {
                    case Log::Level::Trace:
                        Nui::WebApi::Console::trace(message);
                        break;
                    case Log::Level::Debug:
                        Nui::WebApi::Console::debug(message);
                        break;
                    case Log::Level::Info:
                        Nui::WebApi::Console::info(message);
                        break;
                    case Log::Level::Warning:
                        Nui::WebApi::Console::warn(message);
                        break;
                    case Log::Level::Error:
                        Nui::WebApi::Console::error(message);
                        break;
                    case Log::Level::Critical:
                        Nui::WebApi::Console::error(message);
                        break;
                    case Log::Level::Off:
                        break;
                }
            }
        }

        template <typename... Args>
        void log(Log::Level level, std::string_view fmt, Args&&... args)
        {
            if (level < logLevel_)
                return;

            onLog_(std::chrono::system_clock::now(), level, format(fmt, std::forward<Args>(args)...));

            if (logOnConsole_)
            {
                switch (level)
                {
                    case Log::Level::Trace:
                        // Leverage better printing of Nui::val
                        Nui::WebApi::Console::trace(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Debug:
                        Nui::WebApi::Console::debug(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Info:
                        Nui::WebApi::Console::info(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Warning:
                        Nui::WebApi::Console::warn(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Error:
                        Nui::WebApi::Console::error(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Critical:
                        Nui::WebApi::Console::error(fmt, std::forward<Args>(args)...);
                        break;
                    case Log::Level::Off:
                        break;
                    default:
                        break;
                }
            }
        }

        template <typename... Args>
        static std::string format(std::string_view fmt, Args&&... args)
        {
            return fmt::format(fmt::runtime(fmt), Detail::AsFormattable<Args>::convert(std::forward<Args>(args))...);
        }

      private:
        std::function<void(std::chrono::system_clock::time_point const&, Log::Level, std::string const&)> onLog_;
        bool logOnConsole_;
        Nui::RpcClient::AutoUnregister autoUnregisterOnLog_;
        Nui::RpcClient::AutoUnregister autoUnregisterSetLevel_;
        Log::Level logLevel_;
    };
}