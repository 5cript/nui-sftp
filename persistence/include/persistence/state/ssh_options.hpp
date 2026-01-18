#pragma once

#include <persistence/state_core.hpp>

namespace Persistence
{
    struct SshOptions : public DefaultMissingMember
    {
        std::optional<std::filesystem::path> sshDirectory{std::nullopt};
        std::optional<std::filesystem::path> knownHostsFile{std::nullopt};
        std::optional<bool> tryAgentForAuthentication{std::nullopt};
        std::optional<bool> usePublicKeyAutoAuth{std::nullopt};
        std::optional<std::string> logVerbosity{std::nullopt};
        std::optional<std::string> keyExchangeAlgorithms{std::nullopt};
        std::optional<std::string> compressionClientToServer{std::nullopt};
        std::optional<std::string> compressionServerToClient{std::nullopt};
        std::optional<int> compressionLevel{std::nullopt};
        std::optional<bool> strictHostKeyCheck{std::nullopt};
        std::optional<std::string> proxyCommand{std::nullopt};
        std::optional<std::string> gssapiServerIdentity{std::nullopt};
        std::optional<std::string> gssapiClientIdentity{std::nullopt};
        std::optional<bool> gssapiDelegateCredentials{std::nullopt};
        std::optional<bool> noDelay{std::nullopt};
        std::optional<bool> bypassConfig{std::nullopt};
        std::optional<std::string> identityAgent{std::nullopt};
        std::optional<int> connectTimeoutSeconds{std::nullopt};
        std::optional<int> connectTimeoutUSeconds{std::nullopt};
        std::optional<std::map<std::string, std::string>> environment{std::nullopt};
    };
    BOOST_DESCRIBE_STRUCT(
        SshOptions,
        (),
        (sshDirectory,
            knownHostsFile,
            tryAgentForAuthentication,
            usePublicKeyAutoAuth,
            logVerbosity,
            keyExchangeAlgorithms,
            compressionClientToServer,
            compressionServerToClient,
            compressionLevel,
            strictHostKeyCheck,
            proxyCommand,
            gssapiServerIdentity,
            gssapiClientIdentity,
            gssapiDelegateCredentials,
            noDelay,
            bypassConfig,
            identityAgent,
            connectTimeoutSeconds,
            connectTimeoutUSeconds,
            environment)
    )
}