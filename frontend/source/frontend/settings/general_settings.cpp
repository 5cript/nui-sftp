#include <frontend/settings/general_settings.hpp>
#include <frontend/settings/addressable_setting.hpp>
#include <frontend/dialog/input_dialog.hpp>

using namespace std::string_literals;

GeneralSettings::GeneralSettings(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    FrontendEvents* events,
    InputDialog& inputDialog,
    MultiInputDialog& multiInputDialog
)
    : localizationFactory_{factory.within({"generalSettings"})}
    , userInterfaceFactory_{factory.within({"userInterfaceGroupHeader"})}
    , localFilesystemFactory_{factory.within({"localFilesystemOptionsGroupHeader"})}
    , fileTrackingFactory_{factory.within({"fileTrackingOptionsGroupHeader"})}
    , localization{
        .language = {
            {"en_US", "de_DE"},
            localizationFactory_.identity({"general", "localization", "language"}),
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
    }
    , userInterface{
        .theme = ComboSetting<std::string, std::string>{
            { std::string{Constants::defaultThemeName} },
            userInterfaceFactory_.identity({"general", "userInterface", "theme"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "darkLightMode"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "showHiddenFilesLocally"}),
            onChange,
            [this, onChange]()
            {
                userInterface.showHiddenFilesLocally.value(Persistence::UiOptions{}.showHiddenFilesLocally);
                onChange();
            },
        },
        .showHiddenFilesRemotely = BoolSetting<>{
            userInterfaceFactory_.identity({"general", "userInterface", "showHiddenFilesRemotely"}),
            onChange,
            [this, onChange]()
            {
                userInterface.showHiddenFilesRemotely.value(Persistence::UiOptions{}.showHiddenFilesRemotely);
                onChange();
            },
        },
        .fileGridPathBarOnTop = BoolSetting<>{
            userInterfaceFactory_.identity({"general", "userInterface", "fileGridPathBarOnTop"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "fileGridPageSize"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "fileGridExtensionIcons"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "neverShowAgainDialogs"}),
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
            userInterfaceFactory_.identity({"general", "userInterface", "localFavorites"}),
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
                localFilesystemFactory_.identity({"general", "localFilesystemOptions", "preventDeletion"}),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventDeletion.value(Persistence::LocalFilesystemOptions{}.preventDeletion);
                    onChange();
                },
            },
        .preventRename =
            {
                localFilesystemFactory_.identity({"general", "localFilesystemOptions", "preventRename"}),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventRename.value(Persistence::LocalFilesystemOptions{}.preventRename);
                    onChange();
                },
            },
        .preventCreateFile =
            {
                localFilesystemFactory_.identity({"general", "localFilesystemOptions", "preventCreateFile"}),
                onChange,
                [this, onChange]()
                {
                    localFilesystemOptions.preventCreateFile.value(Persistence::LocalFilesystemOptions{}.preventCreateFile);
                    onChange();
                },
            },
        .preventCreateDirectory =
            {
                localFilesystemFactory_.identity({"general", "localFilesystemOptions", "preventCreateDirectory"}),
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
            localFilesystemFactory_.identity({"general", "localFilesystemOptions", "homeOverride"}),
            PathSettingType::Directory,
            onChange,
            [this, onChange]()
            {
                localFilesystemOptions.homeOverride.value(Persistence::LocalFilesystemOptions{}.homeOverride);
                onChange();
            },
        },
        .temporaryDownloadsDirectory = PathSetting<true>{
            localFilesystemFactory_.identity({"general", "localFilesystemOptions", "temporaryDownloadsDirectory"}),
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
            fileTrackingFactory_.identity({"general", "fileTrackingOptions", "autoReupload"}),
            onChange,
            [this, onChange]()
            {
                fileTrackingOptions.autoReupload.value(Persistence::FileTrackingOptions{}.autoReupload);
                onChange();
            },
        },
        .moveRemoteOnLocalMove = BoolSetting<>{
            fileTrackingFactory_.identity({"general", "fileTrackingOptions", "moveRemoteOnLocalMove"}),
            onChange,
            [this, onChange]()
            {
                fileTrackingOptions.moveRemoteOnLocalMove.value(Persistence::FileTrackingOptions{}.moveRemoteOnLocalMove);
                onChange();
            },
        },
        .deleteRemoteOnLocalDelete = BoolSetting<>{
            fileTrackingFactory_.identity({"general", "fileTrackingOptions", "deleteRemoteOnLocalDelete"}),
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
    , logOptions(factory.within({"loggingAndErrorReportingGroupHeader"}), onChange)
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
    state.localizationOptions.dateTimeFormatString = localization.dateTimeFormat;

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
    localization.dateTimeFormat = state.localizationOptions.dateTimeFormatString;

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
            localization.language()
        );

        auto userInterfaceUi = fragment(
            userInterface.theme(),
            userInterface.darkLightMode(),
            userInterface.showHiddenFilesLocally(),
            userInterface.showHiddenFilesRemotely(),
            userInterface.fileGridPathBarOnTop(),
            userInterface.fileGridPageSize(),
            userInterface.fileGridExtensionIcons(),
            userInterface.neverShowAgainDialogs(),
            userInterface.localFavorites()
        );

        auto localFilesystemOptionsUi = fragment(
            localFilesystemOptions.preventDeletion(),
            localFilesystemOptions.preventRename(),
            localFilesystemOptions.preventCreateFile(),
            localFilesystemOptions.preventCreateDirectory(),
            localFilesystemOptions.homeOverride(),
            localFilesystemOptions.temporaryDownloadsDirectory()
        );

        auto fileTrackingOptionsUi = fragment(
            fileTrackingOptions.autoReupload(),
            fileTrackingOptions.moveRemoteOnLocalMove(),
            fileTrackingOptions.deleteRemoteOnLocalDelete()
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