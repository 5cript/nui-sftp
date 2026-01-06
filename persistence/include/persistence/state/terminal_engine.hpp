#pragma once

#include <persistence/state_core.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/ssh_session_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/reference.hpp>
#include <nlohmann/json.hpp>
#include <persistence/state/terminal_options.hpp>

#include <unordered_map>
#include <optional>
#include <vector>
#include <string>
#include <filesystem>
#include <variant>

namespace Persistence
{
    enum class TerminalEngineType
    {
        shell,
        cmd, // TODO
        powershell, // TODO
        ssh
    };
    BOOST_DESCRIBE_ENUM(TerminalEngineType, shell, cmd, powershell, ssh);

    struct BaseTerminalEngine
    {
        bool isPty{true};

        BaseTerminalEngine() = default;
        virtual ~BaseTerminalEngine() = default;
        BaseTerminalEngine(BaseTerminalEngine const&) = default;
        BaseTerminalEngine(BaseTerminalEngine&&) = default;
        BaseTerminalEngine& operator=(BaseTerminalEngine const&) = default;
        BaseTerminalEngine& operator=(BaseTerminalEngine&&) = default;
    };
    BOOST_DESCRIBE_STRUCT(BaseTerminalEngine, (), (isPty))

    struct ExecutingTerminalEngine : BaseTerminalEngine
    {
        std::string command{};
        std::optional<std::vector<std::string>> arguments{std::nullopt};
        std::optional<std::unordered_map<std::string, std::string>> environment{std::nullopt};
        std::optional<int> exitTimeoutSeconds{std::nullopt};
        std::optional<bool> cleanEnvironment{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        ExecutingTerminalEngine,
        (BaseTerminalEngine),
        (command, arguments, environment, exitTimeoutSeconds, cleanEnvironment)
    )

    struct SshTerminalEngine : BaseTerminalEngine
    {
        Referenceable<SshSessionOptions> sshSessionOptions{};
    };
    BOOST_DESCRIBE_STRUCT(SshTerminalEngine, (BaseTerminalEngine), (sshSessionOptions))

    struct TerminalEngine
    {
        std::string type{};
        std::optional<std::string> orderBy{};
        std::optional<bool> startupSession{};
        Referenceable<TerminalOptions> terminalOptions{};
        Referenceable<Termios> termios{};
        std::variant<std::monostate, ExecutingTerminalEngine, SshTerminalEngine> engine{};
        std::optional<std::unordered_map<std::string, nlohmann::json>> layouts{};
        Referenceable<QueueOptions> queueOptions{};

        void variantDecide(nlohmann::json const& j)
        {
            if (j.contains("type"))
            {
                auto typeStr = j["type"].get<std::string>();
                if (typeStr == "ssh")
                    engine = SshTerminalEngine{};
                else if (typeStr == "shell" || typeStr == "cmd" || typeStr == "powershell")
                    engine = ExecutingTerminalEngine{};
                else
                    throw std::runtime_error("Unknown terminal engine type: " + typeStr);
            }
            else
            {
                throw std::runtime_error("Terminal engine type not specified");
            }
        }
    };
    BOOST_DESCRIBE_STRUCT(
        TerminalEngine,
        (),
        (type, orderBy, startupSession, terminalOptions, termios, engine, layouts, queueOptions)
    )

    ExecutingTerminalEngine defaultMsys2TerminalEngine();
    ExecutingTerminalEngine defaultBashTerminalEngine();
}