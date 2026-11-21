#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/ftp_options.hpp>
#include <frontend/settings/ftp_user_settings.hpp>
#include <frontend/settings/remote_server_settings.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
#include <frontend/settings/atomic_setting/multi_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/read_only_text_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>

#include <persistence/state/session_options.hpp>
#include <utility/lifetime_token.hpp>

#include <functional>
#include <string>

/**
 * @brief The name of a TLS mode as the settings and the new session dialog show it.
 */
std::string ftpTlsModeLabel(Persistence::FtpTlsMode tlsMode);

/**
 * @brief Edits the server of an FTP session and the overrides of its FTP options. The transfer options are edited
 * by SessionOptions, which shares them with SSH sessions.
 */
struct FtpSessionOptions
{
  private:
    /**
     * @brief Creates the identities of the host, port and TLS settings.
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
    /**
     * @brief Its port, while unset, shows the port the TLS mode uses by default.
     */
    RemoteServerSettings server;
    ComboSetting<Persistence::FtpTlsMode, std::string> tlsMode;
    /**
     * @brief The public key the backend trusts for the host and port, empty without one. Only shown and forgotten
     *        here: the backend keeps it, and takes it from a certificate that is trusted when connecting.
     */
    ReadOnlyTextSetting trustedPublicKey;
    MultiSetting<FtpUserSettings> users;

    FtpOptions ftpOptions;

    /**
     * @param factory Creates the identities of all settings of the FTP session; it carries no group titles yet.
     */
    FtpSessionOptions(
        SettingFactory const& factory,
        std::function<void()> const& onChange,
        ConfirmDialog& confirmDialog,
        InputDialog& inputDialog
    );

    void applyToState(Persistence::FtpSessionOptions& state) const;
    void loadFromState(Persistence::FtpSessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::FtpSessionOptions const& state);

    /**
     * @brief Renders the server settings in their categories: connection, users and miscellaneous.
     */
    Nui::ElementRenderer renderServerSettings(std::function<void()> const& onChange);

  private:
    /**
     * @brief Shows the default port of the selected TLS mode in the port while no port is set.
     */
    void showDefaultPort();

    /**
     * @brief The change callback of the TLS mode, which also updates the default port shown and the trusted key.
     */
    std::function<void()> tlsModeChanged(std::function<void()> onChange);

    /**
     * @brief The change callback of the host and the port, which also shows the key trusted for the new server.
     */
    std::function<void()> serverChanged(std::function<void()> onChange);

    /**
     * @brief The port a connect would use: the one set, else the default of the TLS mode.
     */
    int effectivePort() const;

    /**
     * @brief Asks the backend for the key it trusts for the host and port, and shows it.
     */
    void showTrustedKey();

    /**
     * @brief Has the backend forget the key it trusts for the host and port, then shows what is left.
     */
    void forgetTrustedKey();

  private:
    Utility::LifetimeToken lifetime_{};
};
