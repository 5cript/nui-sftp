#pragma once

#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>

#include <persistence/state/session_options.hpp>

struct SshSessionOptions
{
    TextSetting<> host;
    NumberSetting<int, true> port;
    TextSetting<true> user;
    PathSetting<true> sshKeyPublic;
    PathSetting<true> sshKeyPrivate;
    BoolSetting<> openSftpByDefault;

    SshOptions sshOptions;
    SftpOptions sftpOptions;

    SshSessionOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::SshSessionOptions& state) const;
    void loadFromState(Persistence::SshSessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SshSessionOptions const& state);
};