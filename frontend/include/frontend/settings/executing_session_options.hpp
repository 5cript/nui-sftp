#pragma once

#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>

#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>

#include <persistence/state/session_options.hpp>

struct ExecutingSessionOptions
{
    BoolSetting<> isPty;
    PathSetting<> command;
    ListSetting<true> arguments;
    MapSetting<true> environment;
    NumberSetting<int> exitTimeoutSeconds;
    BoolSetting<> cleanEnvironment;

    ExecutingSessionOptions(
        std::function<void()> const& onChange,
        InputDialog& inputDialog,
        MultiInputDialog& multiInputDialog
    );

    void applyToState(Persistence::ExecutingSessionOptions& state) const;
    void loadFromState(Persistence::ExecutingSessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::ExecutingSessionOptions const& state);
};