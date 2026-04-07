#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>
#include <frontend/settings/log_options.hpp>
#include <shared_data/theme.hpp>

#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/list_setting.hpp>
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
        Nui::Observed<bool> fileTrackingOptions{true};
    } collapsibleStates;

    struct Localization
    {
        ComboSetting<std::string, std::string> language;
        TextSetting<> dateTimeFormat;
    } localization;

    struct UserInterface
    {
        ComboSetting<std::string, std::string> theme;
        ComboSetting<SharedData::DarkLightMode, std::string> darkLightMode;
        BoolSetting<> showHiddenFilesLocally;
        BoolSetting<> showHiddenFilesRemotely;
        BoolSetting<> fileGridPathBarOnTop;
        MapSetting<> fileGridExtensionIcons;
        ListSetting<false, std::set> neverShowAgainDialogs;
    } userInterface;

    struct LocalFilesystemOptions
    {
        BoolSetting<> preventDeletion;
        BoolSetting<> preventRename;
        BoolSetting<> preventCreateFile;
        BoolSetting<> preventCreateDirectory;
        PathSetting<true> homeOverride;
        PathSetting<true> temporaryDownloadsDirectory;
    } localFilesystemOptions;

    struct FileTrackingOptions
    {
        BoolSetting<> autoReupload;
        BoolSetting<> moveRemoteOnLocalMove;
        BoolSetting<> deleteRemoteOnLocalDelete;
    } fileTrackingOptions;

    LogOptions logOptions;
    Nui::ListenRemover<decltype(AppWideEvents::availableThemes)> availableThemesListener;
    bool darkLightEventOriginatesHere{false};
    Nui::ListenRemover<decltype(FrontendEvents::darkLightMode)> darkLightModeListener;

    GeneralSettings(
        std::function<void()> const& onChange,
        FrontendEvents* events,
        InputDialog& inputDialog,
        MultiInputDialog& multiInputDialog
    );

    void updateThemes(std::vector<std::filesystem::path> paths);

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