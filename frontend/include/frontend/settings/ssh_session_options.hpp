#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
#include <frontend/settings/atomic_setting/multi_setting.hpp>
#include <frontend/settings/ssh_user_settings.hpp>

#include <persistence/state/session_options.hpp>

struct SshSessionOptions
{
  private:
    /**
     * @brief Creates the identities of the host and port settings.
     */
    SettingFactory connectionFactory_;
    /**
     * @brief Creates the identities of the user settings.
     */
    SettingFactory usersFactory_;
    /**
     * @brief Creates the identities of the remaining server settings.
     */
    SettingFactory miscellaneousFactory_;

  public:
    TextSetting<> host;
    NumberSetting<int, true> port;
    MultiSetting<SshUserSettings> users;
    BoolSetting<> openSftpByDefault;
    ListSetting<> remoteFavorites;
    NumberSetting<int, false> maxReconnectAttempts;
    NumberSetting<int, false> maxReconnectBackoffMs;

    SshOptions sshOptions;
    SftpOptions sftpOptions;

    /**
     * @param factory Creates the identities of all settings of the SSH session; it carries no group titles yet.
     */
    SshSessionOptions(
        SettingFactory const& factory,
        std::function<void()> const& onChange,
        ConfirmDialog& confirmDialog,
        InputDialog& inputDialog,
        MultiInputDialog& multiInputDialog
    );

    void applyToState(Persistence::SshSessionOptions& state) const;
    void loadFromState(Persistence::SshSessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SshSessionOptions const& state);

    /**
     * @brief Renders the server settings in their categories: connection, users and miscellaneous.
     */
    Nui::ElementRenderer renderServerSettings(std::function<void()> const& onChange);
};