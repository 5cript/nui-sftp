#pragma once

#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/multi_input_dialog.hpp>

#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/path_setting.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>

struct GeneralSettings
{
    ComboSetting<Log::Level, std::string> logLevel;

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

    GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events, MultiInputDialog& multiInputDialog);
};