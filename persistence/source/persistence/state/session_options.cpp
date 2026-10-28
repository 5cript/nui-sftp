#include <persistence/state/session_options.hpp>

#include <utility/visit_overloaded.hpp>
#include <utility/algorithm/move_to_front.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace Persistence
{
    std::optional<TerminalEngineType> terminalEngineTypeFromName(std::string const& name)
    {
        TerminalEngineType type{};
        if (boost::describe::enum_from_string(name.c_str(), type))
            return type;
        return std::nullopt;
    }

    void SessionOptions::variantDecide(nlohmann::json const& j)
    {
        if (!j.contains("type"))
            throw std::runtime_error("Terminal engine type not specified");
        const auto name = j["type"].get<std::string>();
        const auto type = terminalEngineTypeFromName(name);
        if (!type)
            throw std::runtime_error(fmt::format("Unknown terminal engine type: {}", name));
        switch (*type)
        {
            case TerminalEngineType::ssh:
                engine = SshSessionOptions{};
                return;
            case TerminalEngineType::shell:
                engine = ExecutingSessionOptions{};
                return;
        }
    }

    std::filesystem::path defaultShell()
    {
#ifdef NUI_SFTP_MACOS
        return "/bin/zsh";
#else
        return "/usr/bin/bash";
#endif
    }

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

    ExecutingSessionOptions defaultZshSessionOption()
    {
        ExecutingSessionOptions option{};
        option.command = "/bin/zsh";
        // A login shell like Terminal.app starts, so ~/.zprofile (Homebrew's PATH) is read.
        option.arguments = {std::vector<std::string>{"-l", "-i"}};
        option.environment = {std::map<std::string, std::string>{
            {"TERM", "xterm-256color"},
        }};
        option.exitTimeoutSeconds = 3;
        return option;
    }

    ExecutingSessionOptions defaultShellSessionOption()
    {
#ifdef NUI_SFTP_MACOS
        return defaultZshSessionOption();
#else
        return defaultBashSessionOption();
#endif
    }

    SshUser SshSessionOptions::loginUser() const
    {
        if (users.empty())
            return SshUser{};
        return users.front();
    }

    void SshSessionOptions::writeLegacyUser()
    {
        const auto first = loginUser();
        user = first.name;
        sshKeyPrivate = first.sshKeyPrivate;
        sshKeyPublic = first.sshKeyPublic;
    }

    void SshSessionOptions::selectUser(std::size_t index)
    {
        Utility::Algorithm::moveToFront(users, index);
    }

    void SshSessionOptions::selectUserLike(SshUser const& user)
    {
        const auto iter = std::find_if(
            users.begin(),
            users.end(),
            [&user](SshUser const& candidate)
            {
                return candidate.name == user.name && candidate.sshKeyPrivate == user.sshKeyPrivate;
            }
        );
        if (iter == users.end())
            users.insert(users.begin(), user);
        else
            selectUser(static_cast<std::size_t>(std::distance(users.begin(), iter)));
    }

    SessionOptions SessionOptions::create(std::optional<std::string> icon, TerminalEngineType type)
    {
        if (type == TerminalEngineType::shell)
        {
            return SessionOptions{
                .type = TerminalEngineType::shell,
                .icon = icon.value_or(""),
                .engine = defaultShellSessionOption(),
                .terminalOptions = Reference{"default"},
                .termios = Reference{"default"},
                .queueOptions = Reference{"default"},
                .historyOptions = Reference{"default"},
            };
        }
        return SessionOptions{
            .type = TerminalEngineType::ssh,
            .icon = icon.value_or(""),
            .engine =
                SshSessionOptions{
                    .sshOptions = Reference{"default"},
                    .sftpOptions = Reference{"default"},
                },
            .terminalOptions = Reference{"default"},
            .termios = Reference{"default"},
            .queueOptions = Reference{"default"},
            .historyOptions = Reference{"default"},
        };
    }
}