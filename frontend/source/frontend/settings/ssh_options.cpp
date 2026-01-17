#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>

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
    }
{}