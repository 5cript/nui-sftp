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
        ssh
    };
    BOOST_DESCRIBE_ENUM(TerminalEngineType, shell, ssh);

    struct ExecutingSessionOptions : public DefaultMissingMember
    {
        bool isPty{true};
        std::filesystem::path command{"/usr/bin/bash"};
        std::optional<std::vector<std::string>> arguments{std::nullopt};
        std::optional<std::map<std::string, std::string>> environment{std::nullopt};
        int exitTimeoutSeconds{5};
        bool cleanEnvironment{false};
    };
    BOOST_DESCRIBE_STRUCT(
        ExecutingSessionOptions,
        (),
        (isPty, command, arguments, environment, exitTimeoutSeconds, cleanEnvironment)
    )

    struct SshSessionOptions : public DefaultMissingMember
    {
        std::string host{};
        std::optional<int> port{std::nullopt};
        std::optional<std::string> user{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPrivate{std::nullopt};
        std::optional<std::filesystem::path> sshKeyPublic{std::nullopt};
        bool openSftpByDefault{true};

        // Referenceables:
        Referenceable<SshOptions> sshOptions{};
        Referenceable<SftpOptions> sftpOptions{};
    };
    BOOST_DESCRIBE_STRUCT(
        SshSessionOptions,
        (),
        (sshOptions, sftpOptions, host, port, user, sshKeyPrivate, sshKeyPublic, openSftpByDefault)
    )

    struct SessionOptions : public DefaultMissingMember
    {
        // Generic options:
        TerminalEngineType type{TerminalEngineType::ssh};
        std::string icon{};
        std::optional<std::string> orderBy{};
        bool startupSession{};

        // Engine:
        std::variant<std::monostate, ExecutingSessionOptions, SshSessionOptions> engine{};

        // Layout:
        std::optional<std::map<std::string, nlohmann::json>> layouts{};

        // Referenceables:
        Referenceable<TerminalOptions> terminalOptions{};
        Referenceable<Termios> termios{};
        Referenceable<QueueOptions> queueOptions{};

        void variantDecide(nlohmann::json const& j)
        {
            if (j.contains("type"))
            {
                auto typeStr = j["type"].get<std::string>();
                if (typeStr == "ssh")
                    engine = SshSessionOptions{};
                else if (typeStr == "shell")
                    engine = ExecutingSessionOptions{};
                else
                    throw std::runtime_error("Unknown terminal engine type: " + typeStr);
            }
            else
            {
                throw std::runtime_error("Terminal engine type not specified");
            }
        }

        static SessionOptions create(
            std::optional<std::string> icon = std::nullopt,
            TerminalEngineType type = TerminalEngineType::ssh
        );
    };
    BOOST_DESCRIBE_STRUCT(
        SessionOptions,
        (),
        (type, icon, orderBy, startupSession, terminalOptions, termios, engine, layouts, queueOptions)
    )

    ExecutingSessionOptions defaultMsys2SessionOption();
    ExecutingSessionOptions defaultBashSessionOption();
}