#pragma once

#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/list_setting.hpp>
#include <frontend/settings/map_setting.hpp>
#include <frontend/settings/number_setting.hpp>

#include <persistence/state/session_options.hpp>

struct ExecutingSessionOptions
{
    BoolSetting<> isPty;
    TextSetting<> command;
    ListSetting<true> arguments;
    MapSetting<true> environment;
    NumberSetting<int> exitTimeoutSeconds;
    BoolSetting<> cleanEnvironment;

    ExecutingSessionOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::ExecutingSessionOptions& state) const;
    void loadFromState(Persistence::ExecutingSessionOptions const& state, bool loadRefs);
    void assumeDefaultsFrom(Persistence::ExecutingSessionOptions const& state);
};