#pragma once

#include <log/level.hpp>
#include <log/def.hpp>

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
        Logger()
            : guard_{}
            , rpcHub_{nullptr}
            , stash_{}
            , levelStashed_{Level::Info}
        {}

        static void setupGlobalSinks(Log::Level level, std::filesystem::path const& directory, bool disableFileLog);
        void setup(Nui::RpcHub* hub);
        void setLevel(Log::Level level);
        Log::Level level() const;

        template <typename... Args>
        void log(Log::Level level, std::string_view fmt, Args&&... args)
        {
            const std::string buf = spdlog::fmt_lib::format(spdlog::fmt_lib::runtime(fmt), std::forward<Args>(args)...);
            logImpl(level, buf);
        }

      private:
        void logImpl(Log::Level level, std::string const& msg);

      private:
        std::recursive_mutex guard_;
        Nui::RpcHub* rpcHub_;
        Nui::RpcHub* rpcHubPrelim_;

        // for when the rpc hub is not yet available
        std::vector<std::pair<Log::Level, std::string>> stash_;
        Log::Level levelStashed_;
    };
}