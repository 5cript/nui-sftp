#pragma once

#include <frontend/settings/bool_setting.hpp>

struct QueueOptions
{
    BoolSetting<true> autoRemoveCompletedOperations;
    BoolSetting<true> startInPausedState;

    Nui::Observed<std::string> groupKey{"default"};
    Nui::Observed<std::vector<std::string>> groupKeys{{"default"}};

    QueueOptions(std::function<void()> const& onChange);
};