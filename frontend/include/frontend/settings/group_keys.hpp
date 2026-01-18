#pragma once

#include <nui/event_system/observed_value.hpp>

#include <optional>
#include <string>
#include <vector>

struct GroupKeys
{
    Nui::Observed<std::optional<std::string>> groupKey{std::nullopt};
    Nui::Observed<std::vector<std::string>> groupKeys{};
};