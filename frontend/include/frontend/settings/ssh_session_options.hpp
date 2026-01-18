#pragma once

#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/map_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/text_setting.hpp>

#include <persistence/state/session_options.hpp>

struct SshSessionOptions
{
    TextSetting<> host;
    NumberSetting<int, true> port;
    TextSetting<true> user;
    TextSetting<true> sshKey;
    BoolSetting<> openSftpByDefault;

    SshOptions sshOptions;
    SftpOptions sftpOptions;

    SshSessionOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::SshSessionOptions& state) const;
    void loadFromState(Persistence::SshSessionOptions const& state);
    void assumeDefaultsFrom(Persistence::SshSessionOptions const& state);
};