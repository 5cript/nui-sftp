#pragma once

#include <persistence/state_core.hpp>
#include <log/level.hpp>

#include <optional>
#include <filesystem>

namespace Persistence
{
    struct LogOptions : public DefaultMissingMember
    {
        // by default, dont allow the user to delete files locally.
        Log::Level logLevel{Log::Level::Info};
        std::string logDirectory{"%state_home2%/{appName}/logs"};
        bool disableFileLogging{false};
    };

    BOOST_DESCRIBE_STRUCT(LogOptions, (), (logLevel, logDirectory, disableFileLogging))
}