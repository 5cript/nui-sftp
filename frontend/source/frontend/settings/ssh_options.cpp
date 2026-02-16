#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <utility/enum_string_convert.hpp>

#include <nui/frontend/elements.hpp>

SshOptions::SshOptions(std::function<void()> const& onChange, InputDialog& inputDialog, MultiInputDialog& multiInputDialog)
    : sshDirectory{
        language->getObserved("settings", "sshOptions", "sshDirectoryHelpText"),
        PathSettingType::Directory,
        onChange,
        nulloptReset(sshDirectory, onChange),
    }
    , knownHostsFile{
        language->getObserved("settings", "sshOptions", "knownHostsFileHelpText"),
        PathSettingType::File,
        onChange,
        nulloptReset(knownHostsFile, onChange),
    }
    , tryAgentForAuthentication{
        language->getObserved("settings", "sshOptions", "tryAgentForAuthenticationHelpText"),
        onChange,
        nulloptReset(tryAgentForAuthentication, onChange),
    }
    , usePublicKeyAutoAuth{
        language->getObserved("settings", "sshOptions", "usePublicKeyAutoAuthHelpText"),
        onChange,
        nulloptReset(usePublicKeyAutoAuth, onChange),
    }
    , usePasswordAuth{
        language->getObserved("settings", "sshOptions", "usePasswordAuthHelpText"),
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
        language->getObserved("settings", "sshOptions", "logVerbosityHelpText"),
        onChange,
        nulloptReset(logVerbosity, onChange),
        [](Persistence::SshLogVerbosity const& v)
        {
            return Utility::enumToString(v);
        }
    }
    , keyExchangeAlgorithms{
        language->getObserved("settings", "sshOptions", "keyExchangeAlgorithmsHelpText"),
        onChange,
        nulloptReset(keyExchangeAlgorithms, onChange),
    }
    , compressionClientToServer{
        language->getObserved("settings", "sshOptions", "compressionClientToServerHelpText"),
        onChange,
        nulloptReset(compressionClientToServer, onChange),
    }
    , compressionServerToClient{
        language->getObserved("settings", "sshOptions", "compressionServerToClientHelpText"),
        onChange,
        nulloptReset(compressionServerToClient, onChange),
    }
    , compressionLevel{
        language->getObserved("settings", "sshOptions", "compressionLevelHelpText"),
        onChange,
        nulloptReset(compressionLevel, onChange),
        {
            .minValue = 0,
            .maxValue = 9,
            .asRangeType = true
        }
    }
    , strictHostKeyCheck{
        language->getObserved("settings", "sshOptions", "strictHostKeyCheckHelpText"),
        onChange,
        nulloptReset(strictHostKeyCheck, onChange),
    }
    , proxyCommand{
        language->getObserved("settings", "sshOptions", "proxyCommandHelpText"),
        onChange,
        nulloptReset(proxyCommand, onChange),
    }
    , proxyJump{
        language->getObserved("settings", "sshOptions", "proxyJumpHelpText"),
        onChange,
        nulloptReset(proxyJump, onChange),
    }
    , gssapiServerIdentity{
        language->getObserved("settings", "sshOptions", "gssapiServerIdentityHelpText"),
        onChange,
        nulloptReset(gssapiServerIdentity, onChange),
    }
    , gssapiClientIdentity{
        language->getObserved("settings", "sshOptions", "gssapiClientIdentityHelpText"),
        onChange,
        nulloptReset(gssapiClientIdentity, onChange),
    }
    , gssapiDelegateCredentials{
        language->getObserved("settings", "sshOptions", "gssapiDelegateCredentialsHelpText"),
        onChange,
        nulloptReset(gssapiDelegateCredentials, onChange),
    }
    , noDelay{
        language->getObserved("settings", "sshOptions", "noDelayHelpText"),
        onChange,
        nulloptReset(noDelay, onChange),
    }
    , bypassConfig{
        language->getObserved("settings", "sshOptions", "bypassConfigHelpText"),
        onChange,
        nulloptReset(bypassConfig, onChange),
    }
    , identityAgent{
        language->getObserved("settings", "sshOptions", "identityAgentHelpText"),
        PathSettingType::File,
        onChange,
        nulloptReset(identityAgent, onChange),
    }
    , connectTimeoutSeconds{
        language->getObserved("settings", "sshOptions", "connectTimeoutSecondsHelpText"),
        onChange,
        nulloptReset(connectTimeoutSeconds, onChange),
        {
            .minValue = 0,
            .maxValue = 600,
        }
    }
    , connectTimeoutUSeconds{
        language->getObserved("settings", "sshOptions", "connectTimeoutUSecondsHelpText"),
        onChange,
        nulloptReset(connectTimeoutUSeconds, onChange),
        {
            .minValue = 0,
            .maxValue = 1'000'000,
        }
    },
    environment{
        language->getObserved("settings", "sshOptions", "environmentHelpText"),
        multiInputDialog,
        onChange,
        nulloptReset(environment, onChange)
    },
    identities {
        language->getObserved("settings", "sshOptions", "identitiesHelpText"),
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
    identities.inherit(state.identities);
}

Nui::ElementRenderer SshOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        sshDirectory(language->getObserved("settings", "sshOptions", "sshDirectory")),
        knownHostsFile(language->getObserved("settings", "sshOptions", "knownHostsFile")),
        tryAgentForAuthentication(language->getObserved("settings", "sshOptions", "tryAgentForAuthentication")),
        usePublicKeyAutoAuth(language->getObserved("settings", "sshOptions", "usePublicKeyAutoAuth")),
        usePasswordAuth(language->getObserved("settings", "sshOptions", "usePasswordAuth")),
        logVerbosity(language->getObserved("settings", "sshOptions", "logVerbosity")),
        keyExchangeAlgorithms(language->getObserved("settings", "sshOptions", "keyExchangeAlgorithms")),
        compressionClientToServer(language->getObserved("settings", "sshOptions", "compressionClientToServer")),
        compressionServerToClient(language->getObserved("settings", "sshOptions", "compressionServerToClient")),
        compressionLevel(language->getObserved("settings", "sshOptions", "compressionLevel")),
        strictHostKeyCheck(language->getObserved("settings", "sshOptions", "strictHostKeyCheck")),
        proxyCommand(language->getObserved("settings", "sshOptions", "proxyCommand")),
        proxyJump(language->getObserved("settings", "sshOptions", "proxyJump")),
        gssapiServerIdentity(language->getObserved("settings", "sshOptions", "gssapiServerIdentity")),
        gssapiClientIdentity(language->getObserved("settings", "sshOptions", "gssapiClientIdentity")),
        gssapiDelegateCredentials(language->getObserved("settings", "sshOptions", "gssapiDelegateCredentials")),
        noDelay(language->getObserved("settings", "sshOptions", "noDelay")),
        bypassConfig(language->getObserved("settings", "sshOptions", "bypassConfig")),
        identityAgent(language->getObserved("settings", "sshOptions", "identityAgent")),
        connectTimeoutSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutSeconds")),
        connectTimeoutUSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutUSeconds")),
        environment(language->getObserved("settings", "sshOptions", "environment")),
        identities(language->getObserved("settings", "sshOptions", "identities"))
    );
}