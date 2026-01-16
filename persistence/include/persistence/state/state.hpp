#pragma once

#include <log/level.hpp>
#include <persistence/state_core.hpp>

#include <persistence/state/terminal_engine.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/ssh_session_options.hpp>
#include <persistence/state/ui_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/state/local_filesystem_options.hpp>
#include <persistence/state/localization_options.hpp>

#include <map>
#include <string>

namespace Persistence
{
    struct State : public DefaultMissingMember
    {
        std::map<std::string, TerminalOptions> terminalOptions{};
        std::map<std::string, Termios> termios{};
        std::map<std::string, SshOptions> sshOptions{};
        std::map<std::string, SftpOptions> sftpOptions{};
        std::map<std::string, TerminalEngine> sessions{};
        std::map<std::string, SshSessionOptions> sshSessionOptions{};
        std::map<std::string, QueueOptions> queueOptions{};
        LocalFilesystemOptions localFilesystemOptions{};
        UiOptions uiOptions{};
        Log::Level logLevel{Log::Level::Info};
        LocalizationOptions localizationOptions{};

        State fullyResolve() const;
    };

    BOOST_DESCRIBE_STRUCT(
        State,
        (),
        (terminalOptions,
            sessions,
            termios,
            sshOptions,
            sftpOptions,
            sshSessionOptions,
            uiOptions,
            logLevel,
            localizationOptions,
            queueOptions,
            localFilesystemOptions)
    )
}