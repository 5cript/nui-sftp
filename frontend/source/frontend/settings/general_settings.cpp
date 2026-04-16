#include <frontend/settings/general_settings.hpp>
#include <frontend/dialog/input_dialog.hpp>

using namespace std::string_literals;

GeneralSettings::GeneralSettings(std::function<void()> const& onChange, FrontendEvents* events, InputDialog& inputDialog, MultiInputDialog& multiInputDialog)
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
        .theme = ComboSetting<std::string, std::string>{
            { std::string{Constants::defaultThemeName} },
            language->getObserved("settings", "general", "userInterface", "themeHelpText"),
            [this, events, onChange]()
            {
                events->selectedTheme = userInterface.theme.value();
                events->selectedTheme.modifyNow();
                onChange();
            },
            [this, events, onChange]()
            {
                userInterface.theme.value(std::string{Constants::defaultThemeName});
                events->selectedTheme = userInterface.theme.value();
                events->selectedTheme.modifyNow();
                onChange();
            },
            {},
            {},
            [events](){
                events->onReloadThemes = !events->onReloadThemes;
                return true;
            }
        },
        .darkLightMode = ComboSetting<SharedData::DarkLightMode, std::string>{
            { SharedData::DarkLightMode::System, SharedData::DarkLightMode::Dark, SharedData::DarkLightMode::Light },
            language->getObserved("settings", "general", "userInterface", "darkLightModeHelpText"),
            [this, events, onChange]() {
                Log::info("User changed dark/light mode setting in UI, new value: " + std::to_string(static_cast<int>(userInterface.darkLightMode.value())));
                events->darkLightMode = userInterface.darkLightMode.value();
                events->darkLightMode.eventContext().sync();
            },
            [this, events, onChange]() {
                userInterface.darkLightMode.value(Persistence::UiOptions{}.darkLightMode);
                {
                    darkLightEventOriginatesHere = true;
                    events->darkLightMode = userInterface.darkLightMode.value();
                    events->darkLightMode.eventContext().sync();
                    darkLightEventOriginatesHere = false;
                }
                onChange();
            },
            [](SharedData::DarkLightMode mode) -> std::string
            {
                switch (mode) {
                    case SharedData::DarkLightMode::System:
                        return language->get("settings", "general", "userInterface", "darkLightModeSystem");
                    case SharedData::DarkLightMode::Dark:
                        return language->get("settings", "general", "userInterface", "darkLightModeDark");
                    case SharedData::DarkLightMode::Light:
                        return language->get("settings", "general", "userInterface", "darkLightModeLight");
                }
                return "???";
            },
        },
        .showHiddenFilesLocally = BoolSetting<>{
            language->getObserved("settings", "general", "userInterface", "showHiddenFilesLocallyHelpText"),
            onChange,
            [this, onChange]()
            {
                userInterface.showHiddenFilesLocally.value(Persistence::UiOptions{}.showHiddenFilesLocally);
                onChange();
            },
        },
        .showHiddenFilesRemotely = BoolSetting<>{
            language->getObserved("settings", "general", "userInterface", "showHiddenFilesRemotelyHelpText"),
            onChange,
            [this, onChange]()
            {
                userInterface.showHiddenFilesRemotely.value(Persistence::UiOptions{}.showHiddenFilesRemotely);
                onChange();
            },
        },
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
        .fileGridPageSize = NumberSetting<int, true>{
            language->getObserved("settings", "general", "userInterface", "fileGridPageSizeHelpText"),
            onChange,
            [this, onChange]()
            {
                userInterface.fileGridPageSize.value(Persistence::UiOptions{}.fileGridPageSize);
                onChange();
            },
            NumberSetting<int, true>::ConstructionArgs{
                .minValue = 50,
                .maxValue = 10000,
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
        .neverShowAgainDialogs = ListSetting<false, std::set>{
            language->getObserved("settings", "general", "userInterface", "neverShowAgainDialogsHelpText"),
            inputDialog,
            onChange,
            [this, onChange]()
            {
                userInterface.neverShowAgainDialogs.value(
                    Persistence::UiOptions{}.neverShowAgainDialogs
                );
                onChange();
            }
        },
        .localFavorites = ListSetting<>{
            language->getObserved("settings", "general", "userInterface", "localFavoritesHelpText"),
            inputDialog,
            onChange,
            [this, onChange]()
            {
                userInterface.localFavorites.value(Persistence::UiOptions{}.localFavorites);
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
                localFilesystemOptions.homeOverride.value(Persistence::LocalFilesystemOptions{}.homeOverride);
                onChange();
            },
        },
        .temporaryDownloadsDirectory = PathSetting<true>{
            language->getObserved("settings", "general", "localFilesystemOptions", "temporaryDownloadsDirectoryHelpText"),
            PathSettingType::Directory,
            onChange,
            [this, onChange]()
            {
                localFilesystemOptions.temporaryDownloadsDirectory.value(
                    Persistence::LocalFilesystemOptions{}.temporaryDownloadsDirectory.value_or("%temp%/nui-sftp-downloads")
                );
                onChange();
            },
        },
    }
    , fileTrackingOptions{
        .autoReupload = BoolSetting<>{
            language->getObserved("settings", "general", "fileTrackingOptions", "autoReuploadHelpText"),
            onChange,
            [this, onChange]()
            {
                fileTrackingOptions.autoReupload.value(Persistence::FileTrackingOptions{}.autoReupload);
                onChange();
            },
        },
        .moveRemoteOnLocalMove = BoolSetting<>{
            language->getObserved("settings", "general", "fileTrackingOptions", "moveRemoteOnLocalMoveHelpText"),
            onChange,
            [this, onChange]()
            {
                fileTrackingOptions.moveRemoteOnLocalMove.value(Persistence::FileTrackingOptions{}.moveRemoteOnLocalMove);
                onChange();
            },
        },
        .deleteRemoteOnLocalDelete = BoolSetting<>{
            language->getObserved("settings", "general", "fileTrackingOptions", "deleteRemoteOnLocalDeleteHelpText"),
            onChange,
            [this, onChange]()
            {
                fileTrackingOptions.deleteRemoteOnLocalDelete.value(
                    Persistence::FileTrackingOptions{}.deleteRemoteOnLocalDelete
                );
                onChange();
            },
        },
    }
    , logOptions(onChange)
    , availableThemesListener{
        Nui::smartListen(
            events->availableThemes,
            [this, onChange](auto const& themes)
            {
                updateThemes(themes);
                onChange();
            }
        )
    }
    , darkLightModeListener{
        Nui::smartListen(
            events->darkLightMode,
            [this, onChange](auto value)
            {
                if (!darkLightEventOriginatesHere)
                    this->userInterface.darkLightMode.value(value);
            }
        )
    }
{}

void GeneralSettings::updateThemes(std::vector<std::filesystem::path> paths)
{
    std::vector<std::string> themeNames{{std::string{Constants::defaultThemeName}}};
    for (auto const& themePath : paths)
    {
        const auto name = themePath.filename().stem().string();
        if (name == Constants::defaultThemeName)
            continue;
        themeNames.push_back(name);
    }
    userInterface.theme.options(themeNames);
}

void GeneralSettings::applyToState(Persistence::State& state) const
{
    // Localization Options:
    state.localizationOptions.languageCode = localization.language.value();
    state.localizationOptions.dateTimeFormatString = localization.dateTimeFormat.value();

    // Ui Options
    state.uiOptions.theme = userInterface.theme.value();
    state.uiOptions.darkLightMode = userInterface.darkLightMode.value();
    state.uiOptions.showHiddenFilesLocally = userInterface.showHiddenFilesLocally.value();
    state.uiOptions.showHiddenFilesRemotely = userInterface.showHiddenFilesRemotely.value();
    state.uiOptions.fileGridPathBarOnTop = userInterface.fileGridPathBarOnTop.value();
    if (userInterface.fileGridPageSize.valueIsValid())
        state.uiOptions.fileGridPageSize = userInterface.fileGridPageSize.value().value_or(
            Persistence::UiOptions{}.fileGridPageSize
        );
    state.uiOptions.fileGridExtensionIcons = userInterface.fileGridExtensionIcons.value();
    state.uiOptions.neverShowAgainDialogs = userInterface.neverShowAgainDialogs.value();
    state.uiOptions.localFavorites = userInterface.localFavorites.value();

    // File Tracking
    state.fileTrackingOptions.autoReupload = fileTrackingOptions.autoReupload.value();
    state.fileTrackingOptions.moveRemoteOnLocalMove = fileTrackingOptions.moveRemoteOnLocalMove.value();
    state.fileTrackingOptions.deleteRemoteOnLocalDelete = fileTrackingOptions.deleteRemoteOnLocalDelete.value();

    // Local FS
    state.localFilesystemOptions.preventDeletion = localFilesystemOptions.preventDeletion.value();
    state.localFilesystemOptions.preventRename = localFilesystemOptions.preventRename.value();
    state.localFilesystemOptions.preventCreateFile = localFilesystemOptions.preventCreateFile.value();
    state.localFilesystemOptions.preventCreateDirectory = localFilesystemOptions.preventCreateDirectory.value();
    state.localFilesystemOptions.homeOverride = localFilesystemOptions.homeOverride.value();
    state.localFilesystemOptions.temporaryDownloadsDirectory =
        localFilesystemOptions.temporaryDownloadsDirectory.value();

    logOptions.applyToState(state.logOptions);
}
void GeneralSettings::loadFromState(Persistence::State const& state)
{
    // Localization Options:
    localization.language.value(state.localizationOptions.languageCode);
    localization.dateTimeFormat.value(state.localizationOptions.dateTimeFormatString);

    // Ui Options
    userInterface.theme.value(state.uiOptions.theme);
    userInterface.darkLightMode.value(state.uiOptions.darkLightMode);
    userInterface.showHiddenFilesLocally.value(state.uiOptions.showHiddenFilesLocally);
    userInterface.showHiddenFilesRemotely.value(state.uiOptions.showHiddenFilesRemotely);
    userInterface.fileGridPathBarOnTop.value(state.uiOptions.fileGridPathBarOnTop);
    userInterface.fileGridPageSize.value(state.uiOptions.fileGridPageSize);
    userInterface.fileGridExtensionIcons.value(state.uiOptions.fileGridExtensionIcons);
    userInterface.neverShowAgainDialogs.value(state.uiOptions.neverShowAgainDialogs);
    userInterface.localFavorites.value(state.uiOptions.localFavorites);

    // File Tracking
    fileTrackingOptions.autoReupload.value(state.fileTrackingOptions.autoReupload);
    fileTrackingOptions.moveRemoteOnLocalMove.value(state.fileTrackingOptions.moveRemoteOnLocalMove);
    fileTrackingOptions.deleteRemoteOnLocalDelete.value(state.fileTrackingOptions.deleteRemoteOnLocalDelete);

    // Local FS
    localFilesystemOptions.preventDeletion.value(state.localFilesystemOptions.preventDeletion);
    localFilesystemOptions.preventRename.value(state.localFilesystemOptions.preventRename);
    localFilesystemOptions.preventCreateFile.value(state.localFilesystemOptions.preventCreateFile);
    localFilesystemOptions.preventCreateDirectory.value(state.localFilesystemOptions.preventCreateDirectory);
    localFilesystemOptions.homeOverride.value(state.localFilesystemOptions.homeOverride);
    localFilesystemOptions.temporaryDownloadsDirectory.value(
        state.localFilesystemOptions.temporaryDownloadsDirectory.value_or("%temp%/nui-sftp-downloads")
    );

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
            userInterface.theme(language->getObserved("settings", "general", "userInterface", "theme")),
            userInterface.darkLightMode(language->getObserved("settings", "general", "userInterface", "darkLightMode")),
            userInterface.showHiddenFilesLocally(language->getObserved("settings", "general", "userInterface", "showHiddenFilesLocally")),
            userInterface.showHiddenFilesRemotely(language->getObserved("settings", "general", "userInterface", "showHiddenFilesRemotely")),
            userInterface.fileGridPathBarOnTop(
                language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTop")
            ),
            userInterface.fileGridPageSize(
                language->getObserved("settings", "general", "userInterface", "fileGridPageSize")
            ),
            userInterface.fileGridExtensionIcons(
                language->getObserved("settings", "general", "userInterface", "fileGridExtensionIcons")
            ),
            userInterface.neverShowAgainDialogs(
                language->getObserved("settings", "general", "userInterface", "neverShowAgainDialogs")
            ),
            userInterface.localFavorites(
                language->getObserved("settings", "general", "userInterface", "localFavorites")
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
            ),
            localFilesystemOptions.temporaryDownloadsDirectory(
                language->getObserved("settings", "general", "localFilesystemOptions", "temporaryDownloadsDirectory")
            )
        );

        auto fileTrackingOptionsUi = fragment(
            fileTrackingOptions.autoReupload(
                language->getObserved("settings", "general", "fileTrackingOptions", "autoReupload")
            ),
            fileTrackingOptions.moveRemoteOnLocalMove(
                language->getObserved("settings", "general", "fileTrackingOptions", "moveRemoteOnLocalMove")
            ),
            fileTrackingOptions.deleteRemoteOnLocalDelete(
                language->getObserved("settings", "general", "fileTrackingOptions", "deleteRemoteOnLocalDelete")
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
            }),
            group({
                .isCollapsed = collapsibleStates.fileTrackingOptions,
                .content = std::move(fileTrackingOptionsUi),
                .headerTitle = language->getObserved("settings", "fileTrackingOptionsGroupHeader"),
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