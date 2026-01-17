#include <persistence/state/session_options.hpp>

#include <utility/visit_overloaded.hpp>
#include <nlohmann/json.hpp>

namespace Persistence
{
    ExecutingSessionOptions defaultMsys2SessionOption()
    {
        ExecutingSessionOptions option{};
        option.command = "C:/msys64/usr/bin/bash.exe";
        option.arguments = {std::vector<std::string>{"--login", "-i"}};
        option.environment = {std::map<std::string, std::string>{
            {"MSYSTEM", "MSYS"},
            {"CHERE_INVOKING", "1"},
            {"TERM", "xterm-256color"},
        }};
        option.exitTimeoutSeconds = 3;
        return option;
    }

    ExecutingSessionOptions defaultBashSessionOption()
    {
        ExecutingSessionOptions option{};
        option.command = "/bin/bash";
        option.arguments = {std::vector<std::string>{"-i"}};
        option.environment = {std::map<std::string, std::string>{
            {"TERM", "xterm-256color"},
        }};
        option.exitTimeoutSeconds = 3;
        return option;
    }
}