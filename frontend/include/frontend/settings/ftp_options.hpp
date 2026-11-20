#pragma once

#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/ftp_options.hpp>

#include <functional>
#include <string>

/**
 * @brief Edits one group of the connection options FTP sessions share, or the overrides of one FTP session.
 */
struct FtpOptions : public GroupKeys
{
  private:
    /**
     * @brief Creates the identities of the timeout settings.
     */
    SettingFactory timeoutsFactory_;
    /**
     * @brief Creates the identities of the connection settings.
     */
    SettingFactory connectionsFactory_;
    /**
     * @brief Creates the identities of the settings that keep idle connections alive.
     */
    SettingFactory keepAliveFactory_;
    /**
     * @brief Creates the identities of the TLS settings.
     */
    SettingFactory securityFactory_;

  public:
    NumberSetting<int, true> connectTimeoutSeconds;
    NumberSetting<int, true> commandTimeoutSeconds;
    NumberSetting<int, true> dataInactivityTimeoutSeconds;
    NumberSetting<int, true> transferReplyTimeoutSeconds;
    NumberSetting<int, true> maxConnections;
    ComboSetting<Persistence::FtpExtendedPassive, std::string, true> extendedPassive;
    BoolSetting<true> trustPassiveAddress;
    BoolSetting<true> keepIdleAlive;
    NumberSetting<int, true> idleNoopIntervalSeconds;
    NumberSetting<int, true> keepaliveIdleSeconds;
    NumberSetting<int, true> keepaliveIntervalSeconds;
    NumberSetting<int, true> keepaliveCount;
    ComboSetting<Persistence::FtpTlsVersion, std::string, true> minimumTlsVersion;

    FtpOptions(SettingFactory const& factory, std::function<void()> const& onChange);

    void applyToState(Persistence::FtpOptions& state) const;
    void loadFromState(Persistence::FtpOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::FtpOptions const& state);

    /**
     * @brief Renders the settings in their subgroups: timeouts, connections, keeping alive and security.
     */
    Nui::ElementRenderer render();

  private:
    std::function<void()> onChange_;
};
