#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <utility/language.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

class Settings
{
  public:
    Settings(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Settings);

    Nui::ElementRenderer operator()();

    void applySettingsToUi();

  private:
    Nui::ElementRenderer side();
    Nui::ElementRenderer header();
    Nui::ElementRenderer generalSettings();
    Nui::ElementRenderer sections();

    struct GroupParameters
    {
        Nui::Observed<bool>& isCollapsed;
        Nui::Observed<bool>* isEnabled = nullptr;
        Nui::ElementRenderer content;
        LanguageObservedText headerTitle;
    };
    Nui::ElementRenderer group(GroupParameters&& params);

    enum class Section
    {
        GeneralSettings,
        GlobalInheritables,
        Session,
        Add
    };
    struct SectionSelectorOptions
    {
        Section thisSection{Section::Session};
        std::optional<std::string> sessionId{std::nullopt};
        std::string icon{};
    };
    Nui::ElementRenderer sectionSelector(SectionSelectorOptions const& options);
    bool isActive(SectionSelectorOptions const& options);

    void addNewSession();
    void onChange();
    void applySettingsToState(Persistence::State& state);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};