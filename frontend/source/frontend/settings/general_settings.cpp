#include <frontend/settings/general_settings.hpp>

GeneralSettings::GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events, MultiInputDialog& multiInputDialog)
    : localization{
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
                multiInputDialog,
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