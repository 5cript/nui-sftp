#pragma once

#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/terminal_options.hpp>
#include <frontend/settings/queue_options.hpp>
#include <frontend/settings/history_options.hpp>
#include <frontend/settings/executing_session_options.hpp>
#include <frontend/settings/ssh_session_options.hpp>
#include <frontend/settings/ftp_session_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/state.hpp>
#include <persistence/state/session_options.hpp>

struct SessionOptions
{
  private:
    /**
     * @brief Creates the identities of the basic session settings.
     */
    SettingFactory basicSettingsFactory_;

  public:
    ComboSetting<Persistence::TerminalEngineType, std::string> terminalEngineType;
    ComboSetting<std::string> icon;
    TextSetting<true> orderBy;
    BoolSetting<> isStartupSession;

    TerminalOptions terminalOptions;
    TermiosSettings termios;
    QueueOptions queueOptions;
    HistoryOptions historyOptions;

    // Either may be unused, but we dont want to switch between them by actually destroying
    // the ui. Because of the performance and user feel:
    ExecutingSessionOptions executingSessionOptions;
    SshSessionOptions sshSessionOptions;
    FtpSessionOptions ftpSessionOptions;

    /**
     * @brief The transfer options of SSH and FTP sessions.
     */
    SftpOptions sftpOptions;

    /**
     * @param factory Creates the identities of all settings of the session; it carries no group titles yet.
     */
    SessionOptions(
        SettingFactory const& factory,
        std::function<void()> const& onChange,
        ConfirmDialog& confirmDialog,
        InputDialog& newItemDialog,
        MultiInputDialog& multiInputDialog
    );

    /**
     * @brief Writes the edits into the session. The engine options follow the session type picked here, replacing
     * those of another type.
     */
    void applyToState(Persistence::SessionOptions& state) const;

    /**
     * @brief Loads the session. The editors of the other engine types get the values a new session of their type
     * starts with, so switching the type never shows the values of another session.
     */
    void loadFromState(Persistence::SessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SessionOptions const& state);
};