#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <frontend/settings/log_options.hpp>

#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/setting_group.hpp>

#include <persistence/state/state.hpp>

struct GeneralSettings
{
    struct Collapsibles
    {
        Nui::Observed<bool> localization{false};
        Nui::Observed<bool> logging{false};
        Nui::Observed<bool> userInterface{true};
        Nui::Observed<bool> localFilesystemOptions{true};
    } collapsibleStates;

    struct Localization
    {
        ComboSetting<std::string, std::string> language;
        TextSetting<> dateTimeFormat;
    } localization;

    struct UserInterface
    {
        BoolSetting<> fileGridPathBarOnTop;
        MapSetting<> fileGridExtensionIcons;
    } userInterface;

    struct LocalFilesystemOptions
    {
        BoolSetting<> preventDeletion;
        BoolSetting<> preventRename;
        BoolSetting<> preventCreateFile;
        BoolSetting<> preventCreateDirectory;
        PathSetting<true> homeOverride;
    } localFilesystemOptions;

    LogOptions logOptions;

    GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events, MultiInputDialog& multiInputDialog);

    void applyToState(Persistence::State& state) const;
    void loadFromState(Persistence::State const& state);
    void assumeDefaultsFrom(Persistence::State const& state);
    Nui::ElementRenderer render(
        std::function<void(
            Nui::Observed<std::optional<std::string>>& currentGroupKey,
            Nui::Observed<std::vector<std::string>>& groupKeys
        )> addGroup,
        std::function<void(
            Nui::Observed<std::optional<std::string>>& currentGroupKey,
            Nui::Observed<std::vector<std::string>>& groupKeys
        )> removeGroup,
        std::function<void(
            Nui::Observed<std::optional<std::string>>& currentGroupKey,
            std::optional<std::string> const& newValue,
            Nui::Observed<std::vector<std::string>>& groupKeys,
            SettingGroupParameters::InheritanceBehavior inheritanceBehavior
        )> onChange
    );
};