#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/ssh_options.hpp>

struct SshOptions : public GroupKeys
{
    PathSetting<true> sshDirectory;
    PathSetting<true> knownHostsFile;
    BoolSetting<true> tryAgentForAuthentication;
    BoolSetting<true> usePublicKeyAutoAuth;
    BoolSetting<true> usePasswordAuth;
    ComboSetting<Persistence::SshLogVerbosity, std::string, true> logVerbosity;
    TextSetting<true> keyExchangeAlgorithms;
    BoolSetting<true> compressionClientToServer;
    BoolSetting<true> compressionServerToClient;
    NumberSetting<int, true> compressionLevel;
    BoolSetting<true> strictHostKeyCheck;
    TextSetting<true> proxyCommand;
    TextSetting<true> proxyJump;
    TextSetting<true> gssapiServerIdentity;
    TextSetting<true> gssapiClientIdentity;
    BoolSetting<true> gssapiDelegateCredentials;
    BoolSetting<true> noDelay;
    BoolSetting<true> bypassConfig;
    PathSetting<true> identityAgent;
    NumberSetting<int, true> connectTimeoutSeconds;
    NumberSetting<int, true> connectTimeoutUSeconds;
    MapSetting<true> environment;
    TextSetting<true> localeEnv;
    ListSetting<true> identities;

    SshOptions(std::function<void()> const& onChange, InputDialog& inputDialog, MultiInputDialog& multiInputDialog);

    void applyToState(Persistence::SshOptions& state) const;
    void loadFromState(Persistence::SshOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SshOptions const& state);
    Nui::ElementRenderer render();

  private:
    std::function<void()> onChange_;
};