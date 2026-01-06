#pragma once

#ifndef __EMSCRIPTEN__
#    include <spdlog/spdlog.h>
#endif

#include <utility/algorithm/case_convert.hpp>
#include <utility/describe.hpp>

#include <string>
#include <string_view>

namespace Log
{
    enum class Level
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Critical,
        Off
    };

    BOOST_DESCRIBE_ENUM(Level, Trace, Debug, Info, Warning, Error, Critical, Off);

#ifndef __EMSCRIPTEN__
    inline spdlog::level::level_enum toSpdlogLevel(Level lvl)
    {
        switch (lvl)
        {
            case Level::Trace:
                return spdlog::level::trace;
            case Level::Debug:
                return spdlog::level::debug;
            case Level::Info:
                return spdlog::level::info;
            case Level::Warning:
                return spdlog::level::warn;
            case Level::Error:
                return spdlog::level::err;
            case Level::Critical:
                return spdlog::level::critical;
            case Level::Off:
                return spdlog::level::off;
            default:
                return spdlog::level::info;
        }
    }
    inline Level fromSpdlogLevel(spdlog::level::level_enum lvl)
    {
        switch (lvl)
        {
            case spdlog::level::trace:
                return Level::Trace;
            case spdlog::level::debug:
                return Level::Debug;
            case spdlog::level::info:
                return Level::Info;
            case spdlog::level::warn:
                return Level::Warning;
            case spdlog::level::err:
                return Level::Error;
            case spdlog::level::critical:
                return Level::Critical;
            case spdlog::level::off:
                return Level::Off;
            default:
                return Level::Info;
        }
    }
#endif

    inline Level levelFromString(std::string_view str)
    {
        std::string lowered = Utility::Algorithm::toLowerCase(std::string{str});

        if (lowered == "trace")
            return Level::Trace;
        else if (lowered == "debug")
            return Level::Debug;
        else if (lowered == "info")
            return Level::Info;
        else if (lowered == "warning")
            return Level::Warning;
        else if (lowered == "error")
            return Level::Error;
        else if (lowered == "critical")
            return Level::Critical;
        else if (lowered == "off")
            return Level::Off;
        else
            return Level::Info;
    }

    inline std::string levelToString(Level lvl)
    {
        switch (lvl)
        {
            case Level::Trace:
                return "trace";
            case Level::Debug:
                return "debug";
            case Level::Info:
                return "info";
            case Level::Warning:
                return "warning";
            case Level::Error:
                return "error";
            case Level::Critical:
                return "critical";
            case Level::Off:
                return "off";
            default:
                return "info";
        }
    }
}