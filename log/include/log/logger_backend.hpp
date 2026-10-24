#pragma once

#include <log/level.hpp>
#include <log/def.hpp>
#include <log/toast.hpp>

#include <nui/backend/rpc_hub.hpp>

#include <mutex>
#include <vector>
#include <string>
#include <utility>

namespace Log
{
    class Logger
    {
      public:
        /**
         * @brief A message kept until the frontend logger is ready.
         */
        struct StashedMessage
        {
            Log::Level level;
            Log::Toast toast;
            std::string message;
        };

        Logger()
            : guard_{}
            , rpcHub_{nullptr}
            , rpcHubPrelim_{nullptr}
            , stash_{}
            , levelStashed_{Level::Info}
        {}

        static void setupGlobalSinks(Log::Level level, std::filesystem::path const& directory, bool disableFileLog);
        void setup(Nui::RpcHub* hub);
        void setLevel(Log::Level level);
        Log::Level level() const;

        template <typename... Args>
        void log(Log::Level level, Log::Toast toast, std::string_view fmt, Args&&... args)
        {
            const std::string buf = spdlog::fmt_lib::format(spdlog::fmt_lib::runtime(fmt), std::forward<Args>(args)...);
            logImpl(level, toast, buf);
        }

        template <typename... Args>
        void log(Log::Level level, std::string_view fmt, Args&&... args)
        {
            log(level, Log::Toast::Show, fmt, std::forward<Args>(args)...);
        }

        void detach();

        /**
         * @brief The messages kept until the frontend logger is ready, oldest first.
         */
        std::vector<StashedMessage> stashed() const;

      private:
        void logImpl(Log::Level level, Log::Toast toast, std::string const& msg);

      private:
        mutable std::recursive_mutex guard_;
        Nui::RpcHub* rpcHub_;
        Nui::RpcHub* rpcHubPrelim_;

        // for when the rpc hub is not yet available
        std::vector<StashedMessage> stash_;
        Log::Level levelStashed_;
    };
}