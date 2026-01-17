#pragma once

#include <frontend/events/frontend_events.hpp>

#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/map_setting.hpp>

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
        TextSetting<true> homeOverride;
    } localFilesystemOptions;

    GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events);
};