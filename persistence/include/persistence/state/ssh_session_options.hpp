#pragma once

#include <persistence/state_core.hpp>
#include <persistence/state/ssh_options.hpp>
#include <persistence/state/sftp_options.hpp>
#include <persistence/state/queue_options.hpp>
#include <persistence/reference.hpp>

#include <string>
#include <optional>
#include <filesystem>

namespace Persistence
{
    struct SshSessionOptions
    {
        std::string host{};
        std::optional<int> port{std::nullopt};
        std::optional<std::string> user{std::nullopt};
        // TODO: Remove again. This was only for testing!
        std::optional<std::string> passwordUnsafe{std::nullopt};
        std::optional<std::string> sshKey{std::nullopt};
        std::optional<std::unordered_map<std::string, std::string>> environment{std::nullopt};
        bool openSftpByDefault{true};
        std::optional<std::string> defaultDirectory{std::nullopt};
        Referenceable<SshOptions> sshOptions{};
        Referenceable<SftpOptions> sftpOptions{};
    };

    BOOST_DESCRIBE_STRUCT(
        SshSessionOptions,
        (),
        (host,
            port,
            user,
            passwordUnsafe,
            sshKey,
            environment,
            openSftpByDefault,
            defaultDirectory,
            sshOptions,
            sftpOptions)
    )
}