#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/map_setting.hpp>

#include <persistence/state/ssh_options.hpp>

struct SshOptions : public GroupKeys
{
    TextSetting<true> sshDirectory;
    TextSetting<true> knownHostsFile;
    BoolSetting<true> tryAgentForAuthentication;
    BoolSetting<true> usePublicKeyAutoAuth;
    TextSetting<true> logVerbosity;
    TextSetting<true> keyExchangeAlgorithms;
    TextSetting<true> compressionClientToServer;
    TextSetting<true> compressionServerToClient;
    NumberSetting<int, true> compressionLevel;
    BoolSetting<true> strictHostKeyCheck;
    TextSetting<true> proxyCommand;
    TextSetting<true> gssapiServerIdentity;
    TextSetting<true> gssapiClientIdentity;
    BoolSetting<true> gssapiDelegateCredentials;
    BoolSetting<true> noDelay;
    BoolSetting<true> bypassConfig;
    TextSetting<true> identityAgent;
    NumberSetting<int, true> connectTimeoutSeconds;
    NumberSetting<int, true> connectTimeoutUSeconds;
    MapSetting<true> environment;

    SshOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::SshOptions& state) const;
    void loadFromState(Persistence::SshOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SshOptions const& state);
};