#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <utility/enum_string_convert.hpp>

#include <nui/frontend/elements.hpp>

SshOptions::SshOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    InputDialog& inputDialog,
    MultiInputDialog& multiInputDialog
)
    : factory_{factory.forGroup(groupKey)}
    , sshDirectory{
        factory_.identity({"sshOptions", "sshDirectory"}),
        PathSettingType::Directory,
        onChange,
        nulloptReset(sshDirectory, onChange),
    }
    , knownHostsFile{
        factory_.identity({"sshOptions", "knownHostsFile"}),
        PathSettingType::File,
        onChange,
        nulloptReset(knownHostsFile, onChange),
    }
    , tryAgentForAuthentication{
        factory_.identity({"sshOptions", "tryAgentForAuthentication"}),
        onChange,
        nulloptReset(tryAgentForAuthentication, onChange),
    }
    , usePublicKeyAutoAuth{
        factory_.identity({"sshOptions", "usePublicKeyAutoAuth"}),
        onChange,
        nulloptReset(usePublicKeyAutoAuth, onChange),
    }
    , usePasswordAuth{
        factory_.identity({"sshOptions", "usePasswordAuth"}),
        onChange,
        nulloptReset(usePasswordAuth, onChange),
    }
    , logVerbosity{
        std::vector<Persistence::SshLogVerbosity>{
            Persistence::SshLogVerbosity::Off,
            Persistence::SshLogVerbosity::Warning,
            Persistence::SshLogVerbosity::Protocol,
            Persistence::SshLogVerbosity::Packet,
            Persistence::SshLogVerbosity::Functions
        },
        factory_.identity({"sshOptions", "logVerbosity"}),
        onChange,
        nulloptReset(logVerbosity, onChange),
        [](Persistence::SshLogVerbosity const& v)
        {
            return Utility::enumToString(v);
        }
    }
    , keyExchangeAlgorithms{
        factory_.identity({"sshOptions", "keyExchangeAlgorithms"}),
        onChange,
        nulloptReset(keyExchangeAlgorithms, onChange),
    }
    , compressionClientToServer{
        factory_.identity({"sshOptions", "compressionClientToServer"}),
        onChange,
        nulloptReset(compressionClientToServer, onChange),
    }
    , compressionServerToClient{
        factory_.identity({"sshOptions", "compressionServerToClient"}),
        onChange,
        nulloptReset(compressionServerToClient, onChange),
    }
    , compressionLevel{
        factory_.identity({"sshOptions", "compressionLevel"}),
        onChange,
        nulloptReset(compressionLevel, onChange),
        {
            .minValue = 0,
            .maxValue = 9,
            .asRangeType = true
        }
    }
    , strictHostKeyCheck{
        factory_.identity({"sshOptions", "strictHostKeyCheck"}),
        onChange,
        nulloptReset(strictHostKeyCheck, onChange),
    }
    , proxyCommand{
        factory_.identity({"sshOptions", "proxyCommand"}),
        onChange,
        nulloptReset(proxyCommand, onChange),
    }
    , proxyJump{
        factory_.identity({"sshOptions", "proxyJump"}),
        onChange,
        nulloptReset(proxyJump, onChange),
    }
    , gssapiServerIdentity{
        factory_.identity({"sshOptions", "gssapiServerIdentity"}),
        onChange,
        nulloptReset(gssapiServerIdentity, onChange),
    }
    , gssapiClientIdentity{
        factory_.identity({"sshOptions", "gssapiClientIdentity"}),
        onChange,
        nulloptReset(gssapiClientIdentity, onChange),
    }
    , gssapiDelegateCredentials{
        factory_.identity({"sshOptions", "gssapiDelegateCredentials"}),
        onChange,
        nulloptReset(gssapiDelegateCredentials, onChange),
    }
    , noDelay{
        factory_.identity({"sshOptions", "noDelay"}),
        onChange,
        nulloptReset(noDelay, onChange),
    }
    , bypassConfig{
        factory_.identity({"sshOptions", "bypassConfig"}),
        onChange,
        nulloptReset(bypassConfig, onChange),
    }
    , identityAgent{
        factory_.identity({"sshOptions", "identityAgent"}),
        PathSettingType::File,
        onChange,
        nulloptReset(identityAgent, onChange),
    }
    , connectTimeoutSeconds{
        factory_.identity({"sshOptions", "connectTimeoutSeconds"}),
        onChange,
        nulloptReset(connectTimeoutSeconds, onChange),
        {
            .minValue = 0,
            .maxValue = 600,
        }
    }
    , connectTimeoutUSeconds{
        factory_.identity({"sshOptions", "connectTimeoutUSeconds"}),
        onChange,
        nulloptReset(connectTimeoutUSeconds, onChange),
        {
            .minValue = 0,
            .maxValue = 1'000'000,
        }
    },
    environment{
        factory_.identity({"sshOptions", "environment"}),
        multiInputDialog,
        onChange,
        nulloptReset(environment, onChange)
    },
    localeEnv{
        factory_.identity({"sshOptions", "localeEnv"}),
        onChange,
        nulloptReset(localeEnv, onChange),
    },
    identities {
        factory_.identity({"sshOptions", "identities"}),
        inputDialog,
        onChange,
        nulloptReset(identities, onChange)
    },
    onChange_{onChange}
{}

void SshOptions::applyToState(Persistence::SshOptions& state) const
{
    assignIfValid(state.sshDirectory, sshDirectory);
    assignIfValid(state.knownHostsFile, knownHostsFile);
    assignIfValid(state.tryAgentForAuthentication, tryAgentForAuthentication);
    assignIfValid(state.usePublicKeyAutoAuth, usePublicKeyAutoAuth);
    assignIfValid(state.usePasswordAuth, usePasswordAuth);
    assignIfValid(state.logVerbosity, logVerbosity);
    assignIfValid(state.keyExchangeAlgorithms, keyExchangeAlgorithms);
    assignIfValid(state.compressionClientToServer, compressionClientToServer);
    assignIfValid(state.compressionServerToClient, compressionServerToClient);
    assignIfValid(state.compressionLevel, compressionLevel);
    assignIfValid(state.strictHostKeyCheck, strictHostKeyCheck);
    assignIfValid(state.proxyCommand, proxyCommand);
    assignIfValid(state.proxyJump, proxyJump);
    assignIfValid(state.gssapiServerIdentity, gssapiServerIdentity);
    assignIfValid(state.gssapiClientIdentity, gssapiClientIdentity);
    assignIfValid(state.gssapiDelegateCredentials, gssapiDelegateCredentials);
    assignIfValid(state.noDelay, noDelay);
    assignIfValid(state.bypassConfig, bypassConfig);
    assignIfValid(state.identityAgent, identityAgent);
    assignIfValid(state.connectTimeoutSeconds, connectTimeoutSeconds);
    assignIfValid(state.connectTimeoutUSeconds, connectTimeoutUSeconds);
    assignIfValid(state.environment, environment);
    assignIfValid(state.localeEnv, localeEnv);
    assignIfValid(state.identities, identities);
}

void SshOptions::loadFromState(Persistence::SshOptions const& state, bool)
{
    sshDirectory.value(state.sshDirectory);
    knownHostsFile.value(state.knownHostsFile);
    tryAgentForAuthentication.value(state.tryAgentForAuthentication);
    usePublicKeyAutoAuth.value(state.usePublicKeyAutoAuth);
    usePasswordAuth.value(state.usePasswordAuth);
    logVerbosity.value(state.logVerbosity);
    keyExchangeAlgorithms.value(state.keyExchangeAlgorithms);
    compressionClientToServer.value(state.compressionClientToServer);
    compressionServerToClient.value(state.compressionServerToClient);
    compressionLevel.value(state.compressionLevel);
    strictHostKeyCheck.value(state.strictHostKeyCheck);
    proxyCommand.value(state.proxyCommand);
    proxyJump.value(state.proxyJump);
    gssapiServerIdentity.value(state.gssapiServerIdentity);
    gssapiClientIdentity.value(state.gssapiClientIdentity);
    gssapiDelegateCredentials.value(state.gssapiDelegateCredentials);
    noDelay.value(state.noDelay);
    bypassConfig.value(state.bypassConfig);
    identityAgent.value(state.identityAgent);
    connectTimeoutSeconds.value(state.connectTimeoutSeconds);
    connectTimeoutUSeconds.value(state.connectTimeoutUSeconds);
    environment.value(state.environment);
    localeEnv.value(state.localeEnv);
    identities.value(state.identities);
}

void SshOptions::assumeDefaultsFrom(Persistence::SshOptions const& state)
{
    sshDirectory.inherit(state.sshDirectory);
    knownHostsFile.inherit(state.knownHostsFile);
    tryAgentForAuthentication.inherit(state.tryAgentForAuthentication);
    usePublicKeyAutoAuth.inherit(state.usePublicKeyAutoAuth);
    usePasswordAuth.inherit(state.usePasswordAuth);
    logVerbosity.inherit(state.logVerbosity);
    keyExchangeAlgorithms.inherit(state.keyExchangeAlgorithms);
    compressionClientToServer.inherit(state.compressionClientToServer);
    compressionServerToClient.inherit(state.compressionServerToClient);
    compressionLevel.inherit(state.compressionLevel);
    strictHostKeyCheck.inherit(state.strictHostKeyCheck);
    proxyCommand.inherit(state.proxyCommand);
    proxyJump.inherit(state.proxyJump);
    gssapiServerIdentity.inherit(state.gssapiServerIdentity);
    gssapiClientIdentity.inherit(state.gssapiClientIdentity);
    gssapiDelegateCredentials.inherit(state.gssapiDelegateCredentials);
    noDelay.inherit(state.noDelay);
    bypassConfig.inherit(state.bypassConfig);
    identityAgent.inherit(state.identityAgent);
    connectTimeoutSeconds.inherit(state.connectTimeoutSeconds);
    connectTimeoutUSeconds.inherit(state.connectTimeoutUSeconds);
    environment.inherit(state.environment);
    localeEnv.inherit(state.localeEnv);
    identities.inherit(state.identities);
}

Nui::ElementRenderer SshOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        sshDirectory(),
        knownHostsFile(),
        tryAgentForAuthentication(),
        usePublicKeyAutoAuth(),
        usePasswordAuth(),
        logVerbosity(),
        keyExchangeAlgorithms(),
        compressionClientToServer(),
        compressionServerToClient(),
        compressionLevel(),
        strictHostKeyCheck(),
        proxyCommand(),
        proxyJump(),
        gssapiServerIdentity(),
        gssapiClientIdentity(),
        gssapiDelegateCredentials(),
        noDelay(),
        bypassConfig(),
        identityAgent(),
        connectTimeoutSeconds(),
        connectTimeoutUSeconds(),
        environment(),
        localeEnv(),
        identities()
    );
}