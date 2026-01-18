#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>

SshOptions::SshOptions(std::function<void()> const& onChange)
    : sshDirectory{
            language->getObserved("settings", "sshOptions", "sshDirectoryHelpText"),
            onChange,
            nulloptReset(sshDirectory, onChange),
        }
    , knownHostsFile{
            language->getObserved("settings", "sshOptions", "knownHostsFileHelpText"),
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
    , logVerbosity{
            language->getObserved("settings", "sshOptions", "logVerbosityHelpText"),
            onChange,
            nulloptReset(logVerbosity, onChange),
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
            onChange,
            nulloptReset(identityAgent, onChange),
        }
    , connectTimeoutSeconds{
            language->getObserved("settings", "sshOptions", "connectTimeoutSecondsHelpText"),
            onChange,
            nulloptReset(connectTimeoutSeconds, onChange),
        }
    , connectTimeoutUSeconds{
            language->getObserved("settings", "sshOptions", "connectTimeoutUSecondsHelpText"),
            onChange,
            nulloptReset(connectTimeoutUSeconds, onChange)
    }, environment{
            language->getObserved("settings", "sshOptions", "environmentHelpText"),
            onChange,
            nulloptReset(environment, onChange)
     }
{}

void SshOptions::applyToState(Persistence::SshOptions& state) const
{
    state.sshDirectory = stringOptionalToPathOptional(sshDirectory.value());
    state.knownHostsFile = stringOptionalToPathOptional(knownHostsFile.value());
    state.tryAgentForAuthentication = tryAgentForAuthentication.value();
    state.usePublicKeyAutoAuth = usePublicKeyAutoAuth.value();
    state.logVerbosity = logVerbosity.value();
    state.keyExchangeAlgorithms = keyExchangeAlgorithms.value();
    state.compressionClientToServer = compressionClientToServer.value();
    state.compressionServerToClient = compressionServerToClient.value();
    state.compressionLevel = compressionLevel.value();
    state.strictHostKeyCheck = strictHostKeyCheck.value();
    state.proxyCommand = proxyCommand.value();
    state.gssapiServerIdentity = gssapiServerIdentity.value();
    state.gssapiClientIdentity = gssapiClientIdentity.value();
    state.gssapiDelegateCredentials = gssapiDelegateCredentials.value();
    state.noDelay = noDelay.value();
    state.bypassConfig = bypassConfig.value();
    state.identityAgent = identityAgent.value();
    state.connectTimeoutSeconds = connectTimeoutSeconds.value();
    state.connectTimeoutUSeconds = connectTimeoutUSeconds.value();
    state.environment = environment.value();
}

void SshOptions::loadFromState(Persistence::SshOptions const& state)
{
    sshDirectory.value(pathOptionalToStringOptional(state.sshDirectory));
    knownHostsFile.value(pathOptionalToStringOptional(state.knownHostsFile));
    tryAgentForAuthentication.value(state.tryAgentForAuthentication);
    usePublicKeyAutoAuth.value(state.usePublicKeyAutoAuth);
    logVerbosity.value(state.logVerbosity);
    keyExchangeAlgorithms.value(state.keyExchangeAlgorithms);
    compressionClientToServer.value(state.compressionClientToServer);
    compressionServerToClient.value(state.compressionServerToClient);
    compressionLevel.value(state.compressionLevel);
    strictHostKeyCheck.value(state.strictHostKeyCheck);
    proxyCommand.value(state.proxyCommand);
    gssapiServerIdentity.value(state.gssapiServerIdentity);
    gssapiClientIdentity.value(state.gssapiClientIdentity);
    gssapiDelegateCredentials.value(state.gssapiDelegateCredentials);
    noDelay.value(state.noDelay);
    bypassConfig.value(state.bypassConfig);
    identityAgent.value(state.identityAgent);
    connectTimeoutSeconds.value(state.connectTimeoutSeconds);
    connectTimeoutUSeconds.value(state.connectTimeoutUSeconds);
    environment.value(state.environment);
}

void SshOptions::assumeDefaultsFrom(Persistence::SshOptions const& state)
{
    sshDirectory.inherit(pathOptionalToStringOptional(state.sshDirectory));
    knownHostsFile.inherit(pathOptionalToStringOptional(state.knownHostsFile));
    tryAgentForAuthentication.inherit(state.tryAgentForAuthentication);
    usePublicKeyAutoAuth.inherit(state.usePublicKeyAutoAuth);
    logVerbosity.inherit(state.logVerbosity);
    keyExchangeAlgorithms.inherit(state.keyExchangeAlgorithms);
    compressionClientToServer.inherit(state.compressionClientToServer);
    compressionServerToClient.inherit(state.compressionServerToClient);
    compressionLevel.inherit(state.compressionLevel);
    strictHostKeyCheck.inherit(state.strictHostKeyCheck);
    proxyCommand.inherit(state.proxyCommand);
    gssapiServerIdentity.inherit(state.gssapiServerIdentity);
    gssapiClientIdentity.inherit(state.gssapiClientIdentity);
    gssapiDelegateCredentials.inherit(state.gssapiDelegateCredentials);
    noDelay.inherit(state.noDelay);
    bypassConfig.inherit(state.bypassConfig);
    identityAgent.inherit(state.identityAgent);
    connectTimeoutSeconds.inherit(state.connectTimeoutSeconds);
    connectTimeoutUSeconds.inherit(state.connectTimeoutUSeconds);
    environment.inherit(state.environment);
}