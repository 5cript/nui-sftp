#pragma once

#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>

#include <persistence/state/session_options.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <functional>
#include <string>

/**
 * @brief The settings of one user of an SSH server, edited as an entry of the server's user list.
 */
struct SshUserSettings
{
    using StateType = Persistence::SshUser;

    TextSetting<true> name;
    PathSetting<true> sshKeyPrivate;
    PathSetting<true> sshKeyPublic;

    /**
     * @param hasEntry False while the server has no user to edit, which disables the settings.
     */
    SshUserSettings(
        SettingFactory const& factory,
        std::function<void()> const& onChange,
        Nui::Observed<bool>* hasEntry
    );

    void applyToState(StateType& state) const;
    void loadFromState(StateType const& state);
    Nui::ElementRenderer render();
};

/**
 * @brief Names a user of an SSH server in a select, by its user name and private key file.
 */
std::string sshUserLabel(Persistence::SshUser const& user);
