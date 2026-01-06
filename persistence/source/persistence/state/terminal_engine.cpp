#include <persistence/state/terminal_engine.hpp>

#include <utility/visit_overloaded.hpp>
#include <nlohmann/json.hpp>

namespace Persistence
{
    ExecutingTerminalEngine defaultMsys2TerminalEngine()
    {
        ExecutingTerminalEngine engine{};
        engine.command = "C:/msys64/usr/bin/bash.exe";
        engine.arguments = {std::vector<std::string>{"--login", "-i"}};
        engine.environment = {std::unordered_map<std::string, std::string>{
            {"MSYSTEM", "MSYS"},
            {"CHERE_INVOKING", "1"},
            {"TERM", "xterm-256color"},
        }};
        engine.exitTimeoutSeconds = 3;
        return engine;
    }

    ExecutingTerminalEngine defaultBashTerminalEngine()
    {
        ExecutingTerminalEngine engine{};
        engine.command = "/bin/bash";
        engine.arguments = {std::vector<std::string>{"-i"}};
        engine.environment = {std::unordered_map<std::string, std::string>{
            {"TERM", "xterm-256color"},
        }};
        engine.exitTimeoutSeconds = 3;
        return engine;
    }
}