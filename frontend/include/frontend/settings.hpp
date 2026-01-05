#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>

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

  private:
    Nui::ElementRenderer side();
    Nui::ElementRenderer header();

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

  private:
    void addNewSession();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};