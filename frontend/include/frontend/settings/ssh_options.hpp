#pragma once

#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/number_setting.hpp>

struct SshOptions
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

    Nui::Observed<std::string> groupKey{"default"};
    Nui::Observed<std::vector<std::string>> groupKeys{{"default"}};

    SshOptions(std::function<void()> const& onChange);
};