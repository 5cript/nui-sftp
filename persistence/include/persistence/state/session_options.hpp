#pragma once

#include <persistence/state_core.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/sftp_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/reference.hpp>
#include <nlohmann/json.hpp>
#include <persistence/state/terminal_options.hpp>

#include <map>
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

    struct ExecutingSessionOptions
    {
        bool isPty{true};
        std::string command{};
        std::optional<std::vector<std::string>> arguments{std::nullopt};
        std::optional<std::map<std::string, std::string>> environment{std::nullopt};
        std::optional<int> exitTimeoutSeconds{std::nullopt};
        std::optional<bool> cleanEnvironment{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        ExecutingSessionOptions,
        (),
        (isPty, command, arguments, environment, exitTimeoutSeconds, cleanEnvironment)
    )

    struct SshSessionOptions
    {
        Referenceable<SshOptions> sshOptions{};
        Referenceable<SftpOptions> sftpOptions{};
        std::string host{};
        std::optional<int> port{std::nullopt};
        std::optional<std::string> user{std::nullopt};
        // TODO: Remove again. This was only for testing!
        std::optional<std::string> passwordUnsafe{std::nullopt};
        std::optional<std::string> sshKey{std::nullopt};
        std::optional<std::map<std::string, std::string>> environment{std::nullopt};
        bool openSftpByDefault{true};
        std::optional<std::string> defaultDirectory{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        SshSessionOptions,
        (),
        (sshOptions,
            sftpOptions,
            host,
            port,
            user,
            passwordUnsafe,
            sshKey,
            environment,
            openSftpByDefault,
            defaultDirectory)
    )

    struct SessionOptions
    {
        std::string type{};
        std::optional<std::string> icon{};
        std::optional<std::string> orderBy{};
        std::optional<bool> startupSession{};
        Referenceable<TerminalOptions> terminalOptions{};
        Referenceable<Termios> termios{};
        std::variant<std::monostate, ExecutingSessionOptions, SshSessionOptions> engine{};
        std::optional<std::map<std::string, nlohmann::json>> layouts{};
        Referenceable<QueueOptions> queueOptions{};

        void variantDecide(nlohmann::json const& j)
        {
            if (j.contains("type"))
            {
                auto typeStr = j["type"].get<std::string>();
                if (typeStr == "ssh")
                    engine = SshSessionOptions{};
                else if (typeStr == "shell" || typeStr == "cmd" || typeStr == "powershell")
                    engine = ExecutingSessionOptions{};
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
        SessionOptions,
        (),
        (type, icon, orderBy, startupSession, terminalOptions, termios, engine, layouts, queueOptions)
    )

    ExecutingSessionOptions defaultMsys2SessionOption();
    ExecutingSessionOptions defaultBashSessionOption();
}