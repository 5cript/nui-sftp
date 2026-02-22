#include <frontend/settings/general_settings.hpp>

using namespace std::string_literals;

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
    , userInterface{
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
    }
    , localFilesystemOptions {
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
    , logOptions(onChange)
{}

void GeneralSettings::applyToState(Persistence::State& state) const
{
    // Localization Options:
    state.localizationOptions.languageCode = localization.language.value();
    state.localizationOptions.dateTimeFormatString = localization.dateTimeFormat.value();

    // Ui Options
    state.uiOptions.fileGridPathBarOnTop = userInterface.fileGridPathBarOnTop.value();
    state.uiOptions.fileGridExtensionIcons = userInterface.fileGridExtensionIcons.value();

    // Local FS
    state.localFilesystemOptions.preventDeletion = localFilesystemOptions.preventDeletion.value();
    state.localFilesystemOptions.preventRename = localFilesystemOptions.preventRename.value();
    state.localFilesystemOptions.preventCreateFile = localFilesystemOptions.preventCreateFile.value();
    state.localFilesystemOptions.preventCreateDirectory = localFilesystemOptions.preventCreateDirectory.value();
    state.localFilesystemOptions.homeOverride = localFilesystemOptions.homeOverride.value();

    logOptions.applyToState(state.logOptions);
}
void GeneralSettings::loadFromState(Persistence::State const& state)
{
    // Localization Options:
    localization.language.value(state.localizationOptions.languageCode);
    localization.dateTimeFormat.value(state.localizationOptions.dateTimeFormatString);

    // Ui Options
    userInterface.fileGridPathBarOnTop.value(state.uiOptions.fileGridPathBarOnTop);
    userInterface.fileGridExtensionIcons.value(state.uiOptions.fileGridExtensionIcons);

    // Local FS
    localFilesystemOptions.preventDeletion.value(state.localFilesystemOptions.preventDeletion);
    localFilesystemOptions.preventRename.value(state.localFilesystemOptions.preventRename);
    localFilesystemOptions.preventCreateFile.value(state.localFilesystemOptions.preventCreateFile);
    localFilesystemOptions.preventCreateDirectory.value(state.localFilesystemOptions.preventCreateDirectory);
    localFilesystemOptions.homeOverride.value(state.localFilesystemOptions.homeOverride);

    logOptions.loadFromState(state.logOptions);
}
void GeneralSettings::assumeDefaultsFrom(Persistence::State const&)
{
    // Nothing here.
}
Nui::ElementRenderer GeneralSettings::render(
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
)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    try
    {
        // clang-format off
        auto localizationUi = fragment(
            localization.language(language->getObserved("language"))//,
            //localization.dateTimeFormat(language->getObserved("settings", "general", "localization", "dateTimeFormatString"))
        );

        auto userInterfaceUi = fragment(
            userInterface.fileGridPathBarOnTop(
                language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTop")
            ),
            userInterface.fileGridExtensionIcons(
                language->getObserved("settings", "general", "userInterface", "fileGridExtensionIcons")
            )
        );

        auto localFilesystemOptionsUi = fragment(
            localFilesystemOptions.preventDeletion(
                language->getObserved("settings", "general", "localFilesystemOptions", "preventDeletion")
            ),
            localFilesystemOptions.preventRename(
                language->getObserved("settings", "general", "localFilesystemOptions", "preventRename")
            ),
            localFilesystemOptions.preventCreateFile(
                language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateFile")
            ),
            localFilesystemOptions.preventCreateDirectory(
                language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateDirectory")
            ),
            localFilesystemOptions.homeOverride(
                language->getObserved("settings", "general", "localFilesystemOptions", "homeOverride")
            )
        );
        // clang-format on

        // clang-format off
        return fragment(
            group({
                .isCollapsed = collapsibleStates.localization,
                .content = std::move(localizationUi),
                .headerTitle = language->getObserved("settings", "generalSettings"),
                .addGroup = addGroup,
                .removeGroup = removeGroup,
                .onChangeGroup = onChange,
            }),
            group({
                .isCollapsed = collapsibleStates.logging,
                .content = logOptions.render(),
                .headerTitle = language->getObserved("settings", "loggingAndErrorReportingGroupHeader"),
                .addGroup = addGroup,
                .removeGroup = removeGroup,
                .onChangeGroup = onChange,
            }),
            group({
                .isCollapsed = collapsibleStates.userInterface,
                .content = std::move(userInterfaceUi),
                .headerTitle = language->getObserved("settings", "userInterfaceGroupHeader"),
                .addGroup = addGroup,
                .removeGroup = removeGroup,
                .onChangeGroup = onChange,
            }),
            group({
                .isCollapsed = collapsibleStates.localFilesystemOptions,
                .content = std::move(localFilesystemOptionsUi),
                .headerTitle = language->getObserved("settings", "localFilesystemOptionsGroupHeader"),
                .addGroup = addGroup,
                .removeGroup = removeGroup,
                .onChangeGroup = onChange,
            })
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::generalSettings(): {}", e.what());
        return div{}("Error loading general settings section: "s + e.what());
    }
}