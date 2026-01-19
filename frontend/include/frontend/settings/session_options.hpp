#pragma once

#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/terminal_options.hpp>
#include <frontend/settings/queue_options.hpp>
#include <frontend/settings/executing_session_options.hpp>
#include <frontend/settings/ssh_session_options.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/state.hpp>
#include <persistence/state/session_options.hpp>

struct SessionOptions
{
    ComboSetting<Persistence::TerminalEngineType, std::string> terminalEngineType;
    ComboSetting<std::string> icon;
    TextSetting<true> orderBy;
    BoolSetting<> isStartupSession;

    TerminalOptions terminalOptions;
    TermiosSettings termios;
    QueueOptions queueOptions;

    // Either may be unused, but we dont want to switch between them by actually destroying
    // the ui. Because of the performance and user feel:
    ExecutingSessionOptions executingSessionOptions;
    SshSessionOptions sshSessionOptions;

    SessionOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::SessionOptions& state) const;
    void loadFromState(Persistence::SessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::SessionOptions const& state);
};