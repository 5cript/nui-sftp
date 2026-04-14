#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>

#include <persistence/state/queue_options.hpp>

struct QueueOptions : public GroupKeys
{
    BoolSetting<true> autoRemoveCompletedOperations;
    BoolSetting<true> startInPausedState;
    NumberSetting<int, true> liveQueuePageSize;

    QueueOptions(std::function<void()> const& onChange);

    void applyToState(Persistence::QueueOptions& state) const;
    void loadFromState(Persistence::QueueOptions const& state);
    void assumeDefaultsFrom(Persistence::QueueOptions const& state);
    Nui::ElementRenderer render();
};