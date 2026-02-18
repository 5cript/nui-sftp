#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>

#include <persistence/state/log_options.hpp>

#include <functional>

struct LogOptions : public GroupKeys
{
    ComboSetting<Log::Level, std::string> logLevel;
    TextSetting<> logDirectory;
    BoolSetting<> disableFileLogging;

    LogOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::LogOptions& state) const;
    void loadFromState(Persistence::LogOptions const& state);
    void assumeDefaultsFrom(Persistence::LogOptions const& state);
    Nui::ElementRenderer render();

  private:
    std::function<void()> onChange_;
};