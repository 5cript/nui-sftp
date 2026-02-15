#include <frontend/settings/general_settings.hpp>

#include <svgs/activity-items.hpp>
#include <svgs/zoom-in.hpp>
#include <svgs/information.hpp>
#include <svgs/alert.hpp>
#include <svgs/error.hpp>
#include <svgs/incident.hpp>
#include <svgs/hide.hpp>

GeneralSettings::GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events)
    : logLevel{
        {
            Log::Level::Trace,
            Log::Level::Debug,
            Log::Level::Info,
            Log::Level::Warning,
            Log::Level::Error,
            Log::Level::Critical,
            Log::Level::Off,
        },
        language->getObserved("settings", "general", "loggingAndErrorReporting", "logLevelHelpText"),
        onChange,
        [this, onChange]()
        {
            logLevel.value(Persistence::State{}.logLevel);
            onChange();
        },
        [](Log::Level const& level)
        {
            return Utility::enumToString<Log::Level>(level);
        },
        [](Log::Level const& level) -> Nui::ElementRenderer
        {
            switch (level)
            {
                case Log::Level::Trace:
                    return GeneratedSvgs::activityitems();
                case Log::Level::Debug:
                    return GeneratedSvgs::zoomin();
                case Log::Level::Info:
                    return GeneratedSvgs::information();
                case Log::Level::Warning:
                    return GeneratedSvgs::alert();
                case Log::Level::Error:
                    return GeneratedSvgs::error();
                case Log::Level::Critical:
                    return GeneratedSvgs::incident();
                case Log::Level::Off:
                    return GeneratedSvgs::hide();
                default:
                    return Nui::nil();
            }
        }
    }
    , localization{
            .language = {
                {"en_US", "de_DE"},
                language->getObserved("settings", "general", "localization", "languageHelpText"),
                [onChange, this, events]()
                {
                    onChange();
                    events->onLanguageChanged = localization.language.value();
                    events->onLanguageChanged.modifyNow();
                },
                [this, events, onChange]()
                {
                    localization.language.value(Persistence::State{}.localizationOptions.languageCode);
                    events->onLanguageChanged = localization.language.value();
                    events->onLanguageChanged.modifyNow();
                    onChange();
                },
                [](std::string const& code) -> std::string
                {
                    if (code == "en_US")
                        return "English (US)";
                    else if (code == "de_DE")
                        return "Deutsch";
                    return code;
                },
            },
            .dateTimeFormat = TextSetting<>{
                language->getObserved("settings", "general", "localization", "dateTimeFormatHelpText"),
                onChange,
                [this, onChange]()
                {
                    localization.dateTimeFormat.value(Persistence::State{}.localizationOptions.dateTimeFormatString);
                    onChange();
                },
            },
        }
        ,userInterface{
            .fileGridPathBarOnTop = BoolSetting<>{
                language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTopHelpText"),
                onChange,
                [this, onChange]()
                {
                    userInterface.fileGridPathBarOnTop.value(
                        Persistence::UiOptions{}.fileGridPathBarOnTop
                    );
                    onChange();
                },
            },
            .fileGridExtensionIcons = MapSetting<>{
                language->getObserved("settings", "general", "userInterface", "fileGridExtensionIconsHelpText"),
                onChange,
                [this, onChange]()
                {
                    userInterface.fileGridExtensionIcons.value(
                        Persistence::UiOptions{}.fileGridExtensionIcons
                    );
                    onChange();
                }
            },
        }, localFilesystemOptions
    {
        .preventDeletion =
            {
                language->getObserved("settings", "general", "localFilesystemOptions", "preventDeletionHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventDeletion.value(Persistence::LocalFilesystemOptions{}.preventDeletion);
                    onChange();
                },
            },
        .preventRename =
            {
                language->getObserved("settings", "general", "localFilesystemOptions", "preventRenameHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventRename.value(Persistence::LocalFilesystemOptions{}.preventRename);
                    onChange();
                },
            },
        .preventCreateFile =
            {
                language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateFileHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventCreateFile.value(Persistence::LocalFilesystemOptions{}.preventCreateFile);
                    onChange();
                },
            },
        .preventCreateDirectory =
            {
                language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateDirectoryHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventCreateDirectory.value(
                        Persistence::LocalFilesystemOptions{}.preventCreateDirectory
                    );
                    onChange();
                },
            },
        .homeOverride = PathSetting<true>{
            language->getObserved("settings", "general", "localFilesystemOptions", "homeOverrideHelpText"),
            PathSettingType::Directory,
            onChange,
            [this, onChange]()
            {
                localFilesystemOptions.homeOverride.value(Persistence::LocalFilesystemOptions{}.homeOverride.value_or(""));
                onChange();
            },
        },
    }
{}