#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/history_options.hpp>

#include <string>

/**
 * @brief Settings UI for the command history: how commands are picked up from a terminal.
 *
 * Inheritable like the other referenceable option groups; the effective mode is read once when a
 * session is created, so a change only reaches sessions opened afterwards.
 */
struct HistoryOptions : public GroupKeys
{
    ComboSetting<Persistence::HistoryCaptureMode, std::string, true> captureMode;

    HistoryOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::HistoryOptions& state) const;
    void loadFromState(Persistence::HistoryOptions const& state);
    void assumeDefaultsFrom(Persistence::HistoryOptions const& state);
    Nui::ElementRenderer render();
};
