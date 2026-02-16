#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/session_options.hpp>
#include <utility/language.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <roar/detail/pimpl_special_functions.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>

class Settings
{
  public:
    Settings(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::function<std::optional<nlohmann::json>()> const& obtainCurrentLayout,
        InputDialog& inputDialog,
        ConfirmDialog& confirmDialog,
        MultiInputDialog& multiInputDialog
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Settings);

    Nui::ElementRenderer operator()();

    void applySettingsToUi();

  private:
    Nui::ElementRenderer side();
    Nui::ElementRenderer header();
    Nui::ElementRenderer generalSettings();
    Nui::ElementRenderer inheritableSettings();
    Nui::ElementRenderer currentSession();
    Nui::ElementRenderer sections();

    struct GroupParameters
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
        Persistence::TerminalEngineType engineTypeFilterValue{Persistence::TerminalEngineType::ssh};
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

    void loadTermiosSettingsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state);
    void loadSshSettingsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state);
    void loadSftpOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state);
    void loadTerminalOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state);
    void loadQueueOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state);
    void loadSessionFromState(std::string const& sessionId);

    void applySessionOptionsToState();
    void applySessionToState(std::string const& sessionId);
    void applyReferencesToState();

    void reloadInheritables();
    void reloadInheritance();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};