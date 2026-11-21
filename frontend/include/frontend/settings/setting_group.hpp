#pragma once

#include <persistence/state/state.hpp>
#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <utility/language.hpp>

struct SettingGroupParameters
{
    enum class InheritanceBehavior
    {
        None,
        Inheritable,
        Inheriting
    };

    Nui::Observed<bool>& isCollapsed;
    Nui::ElementRenderer content;
    LanguageObservedText headerTitle;
    Nui::Observed<std::optional<std::string>>* currentGroupKey = nullptr;
    Nui::Observed<std::vector<std::string>>* groupKeys = nullptr;
    InheritanceBehavior inheritanceBehavior = InheritanceBehavior::None;

    Nui::Observed<Persistence::TerminalEngineType>* engineTypeFilter = nullptr;
    /**
     * @brief The engine types the group is shown for, when there is an engineTypeFilter.
     */
    std::vector<Persistence::TerminalEngineType> engineTypeFilterValues{};

    std::function<void(
        Nui::Observed<std::optional<std::string>>& currentGroupKey,
        Nui::Observed<std::vector<std::string>>& groupKeys
    )>
        addGroup{};
    std::function<void(
        Nui::Observed<std::optional<std::string>>& currentGroupKey,
        Nui::Observed<std::vector<std::string>>& groupKeys
    )>
        removeGroup{};
    std::function<void(
        Nui::Observed<std::optional<std::string>>& currentGroupKey,
        std::optional<std::string> const& newValue,
        Nui::Observed<std::vector<std::string>>& groupKeys,
        InheritanceBehavior inheritanceBehavior
    )>
        onChangeGroup{};
};
Nui::ElementRenderer group(SettingGroupParameters&& params);