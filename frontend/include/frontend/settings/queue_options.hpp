#pragma once

#include <frontend/settings/search/setting_factory.hpp>
#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>

#include <persistence/state/queue_options.hpp>

struct QueueOptions : public GroupKeys
{
  private:
    /**
     * @brief Creates the identities of the settings below.
     */
    SettingFactory factory_;

  public:
    BoolSetting<true> autoRemoveCompletedOperations;
    BoolSetting<true> startInPausedState;
    NumberSetting<int, true> liveQueuePageSize;

    QueueOptions(SettingFactory const& factory, std::function<void()> const& onChange);

    void applyToState(Persistence::QueueOptions& state) const;
    void loadFromState(Persistence::QueueOptions const& state);
    void assumeDefaultsFrom(Persistence::QueueOptions const& state);
    Nui::ElementRenderer render();
};