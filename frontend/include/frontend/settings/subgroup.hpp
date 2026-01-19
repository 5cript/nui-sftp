#pragma once

#include <nui/event_system/observed_value.hpp>
#include <utility/language.hpp>
#include <nui/frontend/element_renderer.hpp>

struct SubgroupParameters
{
    Nui::Observed<bool>* engagedStatus = nullptr;
    std::optional<LanguageObservedText> groupTitle = std::nullopt;
    std::function<void()> onChange;
};
Nui::ElementRenderer subgroup(SubgroupParameters&& params, Nui::ElementRenderer content);