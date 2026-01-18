#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/classes.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/queue_options.hpp>
#include <frontend/settings/general_settings.hpp>
#include <frontend/settings/session_options.hpp>
#include <frontend/settings/ssh_options.hpp>
#include <frontend/settings/sftp_options.hpp>
#include <frontend/settings/terminal_options.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/settings/bool_setting.hpp>
#include <frontend/settings/map_setting.hpp>
#include <frontend/settings/number_setting.hpp>
#include <frontend/settings/color_setting.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/button.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/busy_indicator.hpp>
#include <ui5/components/message_strip.hpp>

#include <nui/frontend/api/throttle.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

using namespace std::string_literals;

struct Settings::Implementation
{
    struct CollapsibleStates
    {
        Nui::Observed<bool> localization{false};
        Nui::Observed<bool> loggingAndErrorReporting{false};
        Nui::Observed<bool> userInterface{false};
        Nui::Observed<bool> localFilesystemOptions{false};
        Nui::Observed<bool> sshOptions{false};
        Nui::Observed<bool> sftpOptions{false};
        Nui::Observed<bool> termios{false};
        Nui::Observed<bool> terminalOptions{false};
        Nui::Observed<bool> queueOptions{false};

        struct SessionCollapsibles
        {
            Nui::Observed<bool> overarchingSettings{false};
            Nui::Observed<bool> sshOptions{false};
            Nui::Observed<bool> sftpOptions{false};
        } sessionCollapsibles{};
    } collapsibleStates{};

    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::ThrottledFunction throttledSave{};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};
    Nui::Observed<bool> saveInProgress{false};

    Nui::Observed<std::vector<Settings::SectionSelectorOptions>> sessionSelectors{};

    GeneralSettings generalSettings;
    TermiosSettings termiosSettings;
    SshOptions sshOptions;
    SftpOptions sftpOptions;
    TerminalOptions terminalOptions;
    QueueOptions queueOptions;

    // Values exchange here when switching, we dont have X session option uis, also because performance wise
    // impractical.
    SessionOptions currentSessionOptions;

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        InputDialog* inputDialog,
        ConfirmDialog* confirmDialog,
        std::invocable auto const& onChange
    )
        : stateHolder{stateHolder}
        , events{events}
        , inputDialog{inputDialog}
        , confirmDialog{confirmDialog}
        , generalSettings{onChange, events}
        , termiosSettings{onChange}
        , sshOptions{onChange}
        , sftpOptions{onChange}
        , terminalOptions{onChange}
        , queueOptions{onChange}
        , currentSessionOptions{onChange}
    {}
};

Settings::Settings(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    InputDialog* inputDialog,
    ConfirmDialog* confirmDialog
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          inputDialog,
          confirmDialog,
          [this]()
          {
              if (impl_->throttledSave.valid())
                  impl_->throttledSave();
          }
      )}
{
    Nui::throttle(
        500,
        [this]()
        {
            onChange();
        },
        [this](Nui::ThrottledFunction&& func)
        {
            impl_->throttledSave = std::move(func);
        },
        true
    );

    listen(
        impl_->events->settingsOpen,
        [this](bool open)
        {
            if (open)
                applySettingsToUi();
        }
    );
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Settings);

void Settings::applySettingsToState(Persistence::State& state)
{
    // Uncategorized / General:
    state.logLevel = impl_->generalSettings.logLevel.value();

    // Localization Options:
    state.localizationOptions.languageCode = impl_->generalSettings.localization.language.value();
    state.localizationOptions.dateTimeFormatString = impl_->generalSettings.localization.dateTimeFormat.value();

    // Ui Options
    state.uiOptions.fileGridPathBarOnTop = impl_->generalSettings.userInterface.fileGridPathBarOnTop.value();
    state.uiOptions.fileGridExtensionIcons = impl_->generalSettings.userInterface.fileGridExtensionIcons.value();

    // Local FS
    state.localFilesystemOptions.preventDeletion =
        impl_->generalSettings.localFilesystemOptions.preventDeletion.value();
    state.localFilesystemOptions.preventRename = impl_->generalSettings.localFilesystemOptions.preventRename.value();
    state.localFilesystemOptions.preventCreateFile =
        impl_->generalSettings.localFilesystemOptions.preventCreateFile.value();
    state.localFilesystemOptions.preventCreateDirectory =
        impl_->generalSettings.localFilesystemOptions.preventCreateDirectory.value();
    state.localFilesystemOptions.homeOverride = impl_->generalSettings.localFilesystemOptions.homeOverride.value();

    // Termios
    if (impl_->termiosSettings.groupKey.value())
    {
        Persistence::Termios termiosEntry{};
        impl_->termiosSettings.applyToState(termiosEntry);
        state.termios[*impl_->termiosSettings.groupKey.value()] = std::move(termiosEntry);
    }

    // SSH Options:
    if (impl_->sshOptions.groupKey.value())
    {
        Persistence::SshOptions sshOptionsEntry{};
        impl_->sshOptions.applyToState(sshOptionsEntry);
        state.sshOptions[*impl_->sshOptions.groupKey.value()] = std::move(sshOptionsEntry);
    }

    // SftpOptions:
    if (impl_->sftpOptions.groupKey.value())
    {
        Persistence::SftpOptions sftpOptionsEntry{};
        impl_->sftpOptions.applyToState(sftpOptionsEntry);
        state.sftpOptions[*impl_->sftpOptions.groupKey.value()] = std::move(sftpOptionsEntry);
    }

    // Terminal Options
    if (impl_->terminalOptions.groupKey.value())
    {
        Persistence::TerminalOptions terminalOptionsEntry{};
        impl_->terminalOptions.applyToState(terminalOptionsEntry);
        state.terminalOptions[*impl_->terminalOptions.groupKey.value()] = std::move(terminalOptionsEntry);
    }

    // Queue Options:
    if (impl_->queueOptions.groupKey.value())
    {
        Persistence::QueueOptions queueOptionsEntry{};
        impl_->queueOptions.applyToState(queueOptionsEntry);
        state.queueOptions[*impl_->queueOptions.groupKey.value()] = std::move(queueOptionsEntry);
    }

    // Session Options:
    applySessionOptionsToState();
}

void Settings::applySettingsToUi()
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            impl_->sessionSelectors.value().clear();
            for (auto const& [sessionId, session] : impl_->stateHolder->stateCache().sessions)
            {
                impl_->sessionSelectors.value().push_back(
                    Settings::SectionSelectorOptions{
                        .thisSection = Settings::Section::Session,
                        .sessionId = sessionId,
                        .icon = !session.icon.empty() ? session.icon : "it-system"s,
                    }
                );
            }
            impl_->sessionSelectors.modify();

            impl_->generalSettings.logLevel.value(impl_->stateHolder->stateCache().logLevel);
            impl_->generalSettings.localization.language.value(
                impl_->stateHolder->stateCache().localizationOptions.languageCode
            );
            impl_->generalSettings.localization.dateTimeFormat.value(
                impl_->stateHolder->stateCache().localizationOptions.dateTimeFormatString
            );
            impl_->generalSettings.userInterface.fileGridPathBarOnTop.value(
                impl_->stateHolder->stateCache().uiOptions.fileGridPathBarOnTop
            );
            impl_->generalSettings.userInterface.fileGridExtensionIcons.value(
                impl_->stateHolder->stateCache().uiOptions.fileGridExtensionIcons
            );
            impl_->generalSettings.localFilesystemOptions.preventDeletion.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventDeletion
            );
            impl_->generalSettings.localFilesystemOptions.preventRename.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventRename
            );
            impl_->generalSettings.localFilesystemOptions.preventCreateFile.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventCreateFile
            );
            impl_->generalSettings.localFilesystemOptions.preventCreateDirectory.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.preventCreateDirectory
            );
            impl_->generalSettings.localFilesystemOptions.homeOverride.value(
                impl_->stateHolder->stateCache().localFilesystemOptions.homeOverride.value_or("")
            );

            const auto initialKey = [](auto const& map)
            {
                if (map.empty())
                    return "default"s;
                auto findDefault = map.find("default");
                if (findDefault != map.end())
                    return "default"s;
                return map.begin()->first;
            };
            const auto groupKeys = [](auto const& map)
            {
                std::vector<std::string> keys;
                keys.reserve(map.size());
                for (auto const& [key, _] : map)
                    keys.push_back(key);
                return keys;
            };
            impl_->termiosSettings.groupKey = initialKey(impl_->stateHolder->stateCache().termios);
            impl_->termiosSettings.groupKeys = groupKeys(impl_->stateHolder->stateCache().termios);
            loadTermiosSettingsFromStateByKey(
                impl_->termiosSettings.groupKey.value(), impl_->stateHolder->stateCache()
            );

            impl_->sshOptions.groupKey = initialKey(impl_->stateHolder->stateCache().sshOptions);
            impl_->sshOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().sshOptions);
            loadSshSettingsFromStateByKey(impl_->sshOptions.groupKey.value(), impl_->stateHolder->stateCache());

            impl_->sftpOptions.groupKey = initialKey(impl_->stateHolder->stateCache().sftpOptions);
            impl_->sftpOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().sftpOptions);
            loadSftpOptionsFromStateByKey(impl_->sftpOptions.groupKey.value(), impl_->stateHolder->stateCache());

            impl_->terminalOptions.groupKey = initialKey(impl_->stateHolder->stateCache().terminalOptions);
            impl_->terminalOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().terminalOptions);
            loadTerminalOptionsFromStateByKey(
                impl_->terminalOptions.groupKey.value(), impl_->stateHolder->stateCache()
            );

            impl_->queueOptions.groupKey = initialKey(impl_->stateHolder->stateCache().queueOptions);
            impl_->queueOptions.groupKeys = groupKeys(impl_->stateHolder->stateCache().queueOptions);
            loadQueueOptionsFromStateByKey(impl_->queueOptions.groupKey.value(), impl_->stateHolder->stateCache());

            if (*impl_->activeSection == Section::Session && *impl_->activeSession)
            {
                const auto sessionId = **impl_->activeSession;
                if (impl_->stateHolder->stateCache().sessions.contains(sessionId))
                {
                    impl_->currentSessionOptions.loadFromState(impl_->stateHolder->stateCache().sessions.at(sessionId));
                }
            }

            reloadInheritance();

            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void Settings::loadTermiosSettingsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.termios.contains(*key))
        return;
    impl_->termiosSettings.loadFromState(state.termios.at(*key));
}
void Settings::loadSshSettingsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.sshOptions.contains(*key))
        return;
    impl_->sshOptions.loadFromState(state.sshOptions.at(*key));
}
void Settings::loadSftpOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.sftpOptions.contains(*key))
        return;

    impl_->sftpOptions.loadFromState(state.sftpOptions.at(*key));
}
void Settings::loadTerminalOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.terminalOptions.contains(*key))
        return;
    impl_->terminalOptions.loadFromState(state.terminalOptions.at(*key));
}
void Settings::loadQueueOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.queueOptions.contains(*key))
        return;
    impl_->queueOptions.loadFromState(state.queueOptions.at(*key));
}

void Settings::applySessionOptionsToState()
{
    if (*impl_->activeSection != Section::Session || !*impl_->activeSession)
        return;
    applySessionToState(**impl_->activeSession);
}
void Settings::reloadInheritables()
{
    if (*impl_->activeSection != Section::GlobalInheritables)
        return;

    loadTermiosSettingsFromStateByKey(impl_->termiosSettings.groupKey.value(), impl_->stateHolder->stateCache());
    loadSshSettingsFromStateByKey(impl_->sshOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadSftpOptionsFromStateByKey(impl_->sftpOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadTerminalOptionsFromStateByKey(impl_->terminalOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadQueueOptionsFromStateByKey(impl_->queueOptions.groupKey.value(), impl_->stateHolder->stateCache());
}

void Settings::reloadInheritance()
{
    auto assumeDefaultsFrom = [](auto& setting, auto const& stateMap, std::optional<std::string> const& groupKey)
    {
        if (groupKey)
        {
            auto iter = stateMap.find(*groupKey);
            if (iter != stateMap.end())
                setting.assumeDefaultsFrom(iter->second);
        }
    };

    assumeDefaultsFrom(
        impl_->currentSessionOptions.sshSessionOptions.sshOptions,
        impl_->stateHolder->stateCache().sshOptions,
        *impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKey
    );
    assumeDefaultsFrom(
        impl_->currentSessionOptions.sshSessionOptions.sftpOptions,
        impl_->stateHolder->stateCache().sftpOptions,
        *impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKey
    );
    assumeDefaultsFrom(
        impl_->currentSessionOptions.termios,
        impl_->stateHolder->stateCache().termios,
        *impl_->currentSessionOptions.termios.groupKey
    );
    assumeDefaultsFrom(
        impl_->currentSessionOptions.terminalOptions,
        impl_->stateHolder->stateCache().terminalOptions,
        *impl_->currentSessionOptions.terminalOptions.groupKey
    );
    assumeDefaultsFrom(
        impl_->currentSessionOptions.queueOptions,
        impl_->stateHolder->stateCache().queueOptions,
        *impl_->currentSessionOptions.queueOptions.groupKey
    );
}

void Settings::onChange()
{
    impl_->saveInProgress = true;
    Nui::globalEventContext.executeActiveEventsImmediately();

    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool, Persistence::State const&)
        {
            applySettingsToState(impl_->stateHolder->stateCache());

            impl_->stateHolder->save(
                [this](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .state = ConfirmDialog::State::Negative,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }

                    impl_->saveInProgress = false;
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
            );
        },
        language->get("settings", "errorLoadingSettings")
    );
}

Nui::ElementRenderer Settings::operator()()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    return div{
        class_ = "settings-page-background-blocker",
        style = observe(impl_->events->settingsOpen)
            .generate(
                [](bool isOpen) -> std::string
                {
                    return isOpen ? "display: flex;" : "display: none;";
                }
            ),
    }(impl_->newSessionDialog(),
        div{
            class_ = "settings-page",
        }(header(),
            div{
                class_ = "settings-page-content",
            }(side(),
                div{
                    class_ = "settings-page-content main",
                }(sections()))));
}

Nui::ElementRenderer Settings::sections()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    // clang-format off
    return div{
        class_ = "settings-page-sections"
    }(
        div{style = observe(impl_->activeSection).generate(
            [this]() -> std::string
            {
                return *impl_->activeSection == Section::GeneralSettings ? "" : "display: none;";
            }
        )}(generalSettings()),
        div{style = observe(impl_->activeSection).generate(
            [this]() -> std::string
            {
                return *impl_->activeSection == Section::GlobalInheritables ? "" : "display: none;";
            }
        )}(inheritableSettings()),
        div{style = observe(impl_->activeSection).generate(
            [this]() -> std::string
            {
                return *impl_->activeSection == Section::Session ? "" : "display: none;";
            }
        )}(currentSession())
    );
    // clang-format on
}

Nui::ElementRenderer Settings::header()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = "settings-page-header",
    }(
        iconPanel({
            .name = "action-settings",
            .color = "var(--sapBrandColor)",
            .withBorder = true
        }),
        div{class_ = "title"}(language->getObserved("settings", "title")),
        div{
            class_ = "save-indicator",
            style = observe(impl_->saveInProgress).generate([](bool inProgress) {
                return inProgress ? "visibility: visible;" : "visibility: hidden;";
            })
        }(
            ui5::busy_indicator{
                "size"_prop = "M",
            }(),
            span{}(language->getObserved("settings", "saving"))
        ),
        ui5::button{
            "design"_prop = "Transparent",
            "icon"_prop = "decline",
            "click"_event = [this]() {
                impl_->events->settingsOpen = false;
            },
        }()
    );
    // clang-format on
}

bool Settings::isActive(SectionSelectorOptions const& options)
{
    if (options.thisSection == Section::Session)
        return options.sessionId && *impl_->activeSession == options.sessionId.value_or("");
    else
    {
        return options.thisSection == *impl_->activeSection;
    }
}

void Settings::addNewSession()
{
    impl_->newSessionDialog.open(
        [this](auto const& result)
        {
            Log::info("New session created: {} with icon {}", result.sessionName, result.iconName);
            impl_->stateHolder->stateCache().sessions[result.sessionName] = Persistence::SessionOptions{
                .icon = result.iconName,
            };

            impl_->stateHolder->save(
                [this, result](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .state = ConfirmDialog::State::Negative,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }

                    impl_->sessionSelectors.value().push_back(
                        Settings::SectionSelectorOptions{
                            .thisSection = Settings::Section::Session,
                            .sessionId = result.sessionName,
                            .icon = result.iconName,
                        }
                    );
                    impl_->sessionSelectors.modifyNow();
                }
            );
        }
    );
}

void Settings::loadSessionFromState(std::string const& sessionId)
{
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this, sessionId](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            Nui::WebApi::Console::log("Loading session from state: {}", sessionId);

            auto sessionIter = impl_->stateHolder->stateCache().sessions.find(sessionId);
            if (sessionIter == impl_->stateHolder->stateCache().sessions.end())
                return;
            impl_->currentSessionOptions.loadFromState(sessionIter->second);
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

void Settings::applySessionToState(std::string const& sessionId)
{
    auto sessionIter = impl_->stateHolder->stateCache().sessions.find(sessionId);
    if (sessionIter == impl_->stateHolder->stateCache().sessions.end())
    {
        Log::error("Tried to apply session settings to state for unknown session id: {}.", sessionId);
        return;
    }
    impl_->currentSessionOptions.applyToState(sessionIter->second);
}

Nui::ElementRenderer Settings::sectionSelector(SectionSelectorOptions const& options)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = observe(impl_->activeSection, impl_->activeSession).generate(
            [this, options]()
            {
                return fmt::format("settings-page-section-selector {}", isActive(options) ? "active" : "");
            }
        ),
        onClick = [this, options]() {
            if (options.thisSection == Section::Add) {
                addNewSession();
                return;
            }

            if (impl_->activeSession.value())
                applySessionToState(*impl_->activeSession.value());
            if (options.thisSection == Section::Session && options.sessionId.has_value()) {
                impl_->activeSection = Section::Session;
                impl_->activeSession = options.sessionId;
                loadSessionFromState(options.sessionId.value_or(""));
            } else {
                impl_->activeSection = options.thisSection;
                impl_->activeSession = std::nullopt;
            }
        },
    }(
        observe(impl_->activeSection, impl_->activeSession),
        [this, options](){
            const auto active = isActive(options);

            return fragment(
                [icon = options.icon, active]() -> Nui::ElementRenderer {
                    if (icon.empty())
                        return Nui::nil();
                    return iconPanel({
                        .name = icon,
                        .color = active ? "var(--sapBrandColor)" : "#404040",
                        .withBorder = true
                    });
                }(),
                span{}(
                    observe(impl_->events->onLanguageChanged).generate(
                        [&options]() -> std::string {
                        if (options.sessionId.has_value())
                            return options.sessionId.value();

                        switch (options.thisSection) {
                            case Settings::Section::GeneralSettings:
                                return language->get("settings", "generalSettings");
                            case Settings::Section::GlobalInheritables:
                                return language->get("settings", "globalInheritables");
                            case Settings::Section::Session:
                                return language->get("settings", "unknownSession");
                            case Settings::Section::Add:
                                return language->get("settings", "addNew");
                        }
                    })
                )
            );
        }
    );
    // clang-format on
}

Nui::ElementRenderer Settings::side()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{class_ = "side"}(
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "settings",
                "design"_prop = "Neutral"
            }(),
            span{}(language->getObserved("settings", "configuration"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::GeneralSettings,
            .icon = "wrench",
        }),
        sectionSelector({
            .thisSection = Settings::Section::GlobalInheritables,
            .icon = "settings",
        }),
        div{style = "width: calc(100% - 20px); border-top: 1px solid gray; margin-bottom: 20px; margin-top: 10px"}(),
        div{class_ = "configuration-text"}(
            ui5::icon{
                "name"_prop = "it-system",
                "design"_prop = "Neutral"
            }(),
            span{}(language->getObserved("settings", "sessionsServers"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::Add,
            .icon = "add",
        }),
        div{style = "display: flex; flex-direction: column;"}(
            impl_->sessionSelectors.map([this](long long, auto const& item) -> Nui::ElementRenderer {
                return sectionSelector({
                    .thisSection = Settings::Section::Session,
                    .sessionId = item.sessionId,
                    .icon = item.icon,
                });
            })
        )
    );
    // clang-format on
}

Nui::ElementRenderer Settings::generalSettings()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    auto loggingAndErrorReporting = fragment(
        impl_->generalSettings.logLevel(language->getObserved("settings", "logLevel"))
    );

    auto localization = fragment(
        impl_->generalSettings.localization.language(language->getObserved("language")),
        impl_->generalSettings.localization.dateTimeFormat(language->getObserved("settings", "general", "localization", "dateTimeFormatString"))
    );

    auto userInterface = fragment(
        impl_->generalSettings.userInterface.fileGridPathBarOnTop(
            language->getObserved("settings", "general", "userInterface", "fileGridPathBarOnTop")
        ),
        impl_->generalSettings.userInterface.fileGridExtensionIcons(
            language->getObserved("settings", "general", "userInterface", "fileGridExtensionIcons")
        )
    );

    auto localFilesystemOptions = fragment(
        impl_->generalSettings.localFilesystemOptions.preventDeletion(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventDeletion")
        ),
        impl_->generalSettings.localFilesystemOptions.preventRename(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventRename")
        ),
        impl_->generalSettings.localFilesystemOptions.preventCreateFile(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateFile")
        ),
        impl_->generalSettings.localFilesystemOptions.preventCreateDirectory(
            language->getObserved("settings", "general", "localFilesystemOptions", "preventCreateDirectory")
        ),
        impl_->generalSettings.localFilesystemOptions.homeOverride(
            language->getObserved("settings", "general", "localFilesystemOptions", "homeOverride")
        )
    );
    // clang-format on

    // clang-format off
    return fragment(
        group({
            .isCollapsed = impl_->collapsibleStates.localization,
            .content = std::move(localization),
            .headerTitle = language->getObserved("settings", "generalSettings")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.loggingAndErrorReporting,
            .content = std::move(loggingAndErrorReporting),
            .headerTitle = language->getObserved("settings", "loggingAndErrorReportingGroupHeader")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.userInterface,
            .content = std::move(userInterface),
            .headerTitle = language->getObserved("settings", "userInterfaceGroupHeader")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.localFilesystemOptions,
            .content = std::move(localFilesystemOptions),
            .headerTitle = language->getObserved("settings", "localFilesystemOptionsGroupHeader")
        })
    );
    // clang-format on
}

Nui::ElementRenderer Settings::inheritableSettings()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    auto sshOptions = fragment(
        impl_->sshOptions.sshDirectory(language->getObserved("settings", "sshOptions", "sshDirectory")),
        impl_->sshOptions.knownHostsFile(language->getObserved("settings", "sshOptions", "knownHostsFile")),
        impl_->sshOptions.tryAgentForAuthentication(language->getObserved("settings", "sshOptions", "tryAgentForAuthentication")),
        impl_->sshOptions.usePublicKeyAutoAuth(language->getObserved("settings", "sshOptions", "usePublicKeyAutoAuth")),
        impl_->sshOptions.logVerbosity(language->getObserved("settings", "sshOptions", "logVerbosity")),
        impl_->sshOptions.keyExchangeAlgorithms(language->getObserved("settings", "sshOptions", "keyExchangeAlgorithms")),
        impl_->sshOptions.compressionClientToServer(language->getObserved("settings", "sshOptions", "compressionClientToServer")),
        impl_->sshOptions.compressionServerToClient(language->getObserved("settings", "sshOptions", "compressionServerToClient")),
        impl_->sshOptions.compressionLevel(language->getObserved("settings", "sshOptions", "compressionLevel")),
        impl_->sshOptions.strictHostKeyCheck(language->getObserved("settings", "sshOptions", "strictHostKeyCheck")),
        impl_->sshOptions.proxyCommand(language->getObserved("settings", "sshOptions", "proxyCommand")),
        impl_->sshOptions.gssapiServerIdentity(language->getObserved("settings", "sshOptions", "gssapiServerIdentity")),
        impl_->sshOptions.gssapiClientIdentity(language->getObserved("settings", "sshOptions", "gssapiClientIdentity")),
        impl_->sshOptions.gssapiDelegateCredentials(language->getObserved("settings", "sshOptions", "gssapiDelegateCredentials")),
        impl_->sshOptions.noDelay(language->getObserved("settings", "sshOptions", "noDelay")),
        impl_->sshOptions.bypassConfig(language->getObserved("settings", "sshOptions", "bypassConfig")),
        impl_->sshOptions.identityAgent(language->getObserved("settings", "sshOptions", "identityAgent")),
        impl_->sshOptions.connectTimeoutSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutSeconds")),
        impl_->sshOptions.connectTimeoutUSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutUSeconds"))
    );
    // clang-format on

    // clang-format off
    auto sftpOptions = fragment(
        subgroup({
            .engagedStatus = &impl_->sftpOptions.downloadOptionsEngaged,
            .groupTitle = language->getObserved("settings", "sftpOptions", "downloadOptionsSubgroupTitle")
        }, fragment(
            impl_->sftpOptions.downloadOptions.tempFileSuffix(language->getObserved("settings", "sftpOptions", "downloadOptions", "tempFileSuffix")),
            impl_->sftpOptions.downloadOptions.mayOverwrite(language->getObserved("settings", "sftpOptions", "downloadOptions", "mayOverwrite")),
            impl_->sftpOptions.downloadOptions.tryContinue(language->getObserved("settings", "sftpOptions", "downloadOptions", "tryContinue")),
            impl_->sftpOptions.downloadOptions.inheritPermissions(language->getObserved("settings", "sftpOptions", "downloadOptions", "inheritPermissions")),
            impl_->sftpOptions.downloadOptions.customPermissions(language->getObserved("settings", "sftpOptions", "downloadOptions", "customPermissions")),
            impl_->sftpOptions.downloadOptions.reserveSpace(language->getObserved("settings", "sftpOptions", "downloadOptions", "reserveSpace")),
            impl_->sftpOptions.downloadOptions.doCleanup(language->getObserved("settings", "sftpOptions", "downloadOptions", "doCleanup"))
        )),
        subgroup({
            .engagedStatus = &impl_->sftpOptions.uploadOptionsEngaged,
            .groupTitle = language->getObserved("settings", "sftpOptions", "uploadOptionsSubgroupTitle")
        }, fragment(
            impl_->sftpOptions.uploadOptions.tempFileSuffix(language->getObserved("settings", "sftpOptions", "uploadOptions", "tempFileSuffix")),
            impl_->sftpOptions.uploadOptions.mayOverwrite(language->getObserved("settings", "sftpOptions", "uploadOptions", "mayOverwrite")),
            impl_->sftpOptions.uploadOptions.tryContinue(language->getObserved("settings", "sftpOptions", "uploadOptions", "tryContinue")),
            impl_->sftpOptions.uploadOptions.inheritPermissions(language->getObserved("settings", "sftpOptions", "uploadOptions", "inheritPermissions")),
            impl_->sftpOptions.uploadOptions.customPermissions(language->getObserved("settings", "sftpOptions", "uploadOptions", "customPermissions"))
        )),
        impl_->sftpOptions.concurrency(language->getObserved("settings", "sftpOptions", "concurrency")),
        impl_->sftpOptions.operationTimeoutSeconds(language->getObserved("settings", "sftpOptions", "operationTimeoutSeconds"))
    );
    // clang-format on

    // clang-format off
    auto terminalOptions = fragment(
        impl_->terminalOptions.fontFamily(language->getObserved("settings", "terminalOptions", "fontFamily")),
        impl_->terminalOptions.fontSize(language->getObserved("settings", "terminalOptions", "fontSize")),
        impl_->terminalOptions.lineHeight(language->getObserved("settings", "terminalOptions", "lineHeight")),
        impl_->terminalOptions.cursorBlink(language->getObserved("settings", "terminalOptions", "cursorBlink")),
        impl_->terminalOptions.renderer(language->getObserved("settings", "terminalOptions", "renderer")),
        impl_->terminalOptions.letterSpacing(language->getObserved("settings", "terminalOptions", "letterSpacing")),
        subgroup({
            .engagedStatus = &impl_->terminalOptions.themeEngaged,
            .groupTitle = language->getObserved("settings", "terminalOptions", "themeSubgroupTitle")
        }, fragment(
            impl_->terminalOptions.theme.background(language->getObserved("settings", "terminalOptions", "theme", "background")),
            impl_->terminalOptions.theme.black(language->getObserved("settings", "terminalOptions", "theme", "black")),
            impl_->terminalOptions.theme.blue(language->getObserved("settings", "terminalOptions", "theme", "blue")),
            impl_->terminalOptions.theme.brightBlack(language->getObserved("settings", "terminalOptions", "theme", "brightBlack")),
            impl_->terminalOptions.theme.brightBlue(language->getObserved("settings", "terminalOptions", "theme", "brightBlue")),
            impl_->terminalOptions.theme.brightCyan(language->getObserved("settings", "terminalOptions", "theme", "brightCyan")),
            impl_->terminalOptions.theme.brightGreen(language->getObserved("settings", "terminalOptions", "theme", "brightGreen")),
            impl_->terminalOptions.theme.brightMagenta(language->getObserved("settings", "terminalOptions", "theme", "brightMagenta")),
            impl_->terminalOptions.theme.brightRed(language->getObserved("settings", "terminalOptions", "theme", "brightRed")),
            impl_->terminalOptions.theme.brightWhite(language->getObserved("settings", "terminalOptions", "theme", "brightWhite")),
            impl_->terminalOptions.theme.brightYellow(language->getObserved("settings", "terminalOptions", "theme", "brightYellow")),
            impl_->terminalOptions.theme.cursor(language->getObserved("settings", "terminalOptions", "theme", "cursor")),
            impl_->terminalOptions.theme.cursorAccent(language->getObserved("settings", "terminalOptions", "theme", "cursorAccent")),
            impl_->terminalOptions.theme.cyan(language->getObserved("settings", "terminalOptions", "theme", "cyan")),
            impl_->terminalOptions.theme.foreground(language->getObserved("settings", "terminalOptions", "theme", "foreground")),
            impl_->terminalOptions.theme.green(language->getObserved("settings", "terminalOptions", "theme", "green")),
            impl_->terminalOptions.theme.magenta(language->getObserved("settings", "terminalOptions", "theme", "magenta")),
            impl_->terminalOptions.theme.red(language->getObserved("settings", "terminalOptions", "theme", "red")),
            impl_->terminalOptions.theme.selectionBackground(language->getObserved("settings", "terminalOptions", "theme", "selectionBackground")),
            impl_->terminalOptions.theme.selectionForeground(language->getObserved("settings", "terminalOptions", "theme", "selectionForeground")),
            impl_->terminalOptions.theme.selectionInactiveBackground(language->getObserved("settings", "terminalOptions", "theme", "selectionInactiveBackground")),
            impl_->terminalOptions.theme.white(language->getObserved("settings", "terminalOptions", "theme", "white")),
            impl_->terminalOptions.theme.yellow(language->getObserved("settings", "terminalOptions", "theme", "yellow"))
        ))
    );
    // clang-format on

    // clang-format off
    auto queueOptions = fragment(
        impl_->queueOptions.autoRemoveCompletedOperations(language->getObserved("settings", "queueOptions", "autoRemoveCompletedOperations")),
        impl_->queueOptions.startInPausedState(language->getObserved("settings", "queueOptions", "startInPausedState"))
    );
    // clang-format on

    // clang-format off
    auto termios = fragment(
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "inputFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.inputFlags.IGNBRK("IGNBRK"),
            impl_->termiosSettings.inputFlags.BRKINT("BRKINT"),
            impl_->termiosSettings.inputFlags.IGNPAR("IGNPAR"),
            impl_->termiosSettings.inputFlags.PARMRK("PARMRK"),
            impl_->termiosSettings.inputFlags.INPCK("INPCK"),
            impl_->termiosSettings.inputFlags.ISTRIP("ISTRIP"),
            impl_->termiosSettings.inputFlags.INLCR("INLCR"),
            impl_->termiosSettings.inputFlags.IGNCR("IGNCR"),
            impl_->termiosSettings.inputFlags.ICRNL("ICRNL"),
            impl_->termiosSettings.inputFlags.IUCLC("IUCLC"),
            impl_->termiosSettings.inputFlags.IXON("IXON"),
            impl_->termiosSettings.inputFlags.IXANY("IXANY"),
            impl_->termiosSettings.inputFlags.IXOFF("IXOFF"),
            impl_->termiosSettings.inputFlags.IMAXBEL("IMAXBEL"),
            impl_->termiosSettings.inputFlags.IUTF8("IUTF8")
        )),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "outputFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.outputFlags.OPOST("OPOST"),
            impl_->termiosSettings.outputFlags.OLCUC("OLCUC"),
            impl_->termiosSettings.outputFlags.ONLCR("ONLCR"),
            impl_->termiosSettings.outputFlags.OCRNL("OCRNL"),
            impl_->termiosSettings.outputFlags.ONOCR("ONOCR"),
            impl_->termiosSettings.outputFlags.ONLRET("ONLRET"),
            impl_->termiosSettings.outputFlags.OFILL("OFILL"),
            impl_->termiosSettings.outputFlags.OFDEL("OFDEL"),
            impl_->termiosSettings.outputFlags.NLDLY("NLDLY"),
            impl_->termiosSettings.outputFlags.CRDLY("CRDLY"),
            impl_->termiosSettings.outputFlags.TABDLY("TABDLY"),
            impl_->termiosSettings.outputFlags.BSDLY("BSDLY"),
            impl_->termiosSettings.outputFlags.VTDLY("VTDLY"),
            impl_->termiosSettings.outputFlags.FFDLY("FFDLY")
        )),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "controlFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.controlFlags.CBAUD("CBAUD"),
            impl_->termiosSettings.controlFlags.CBAUDEX("CBAUDEX"),
            impl_->termiosSettings.controlFlags.CSIZE("CSIZE"),
            impl_->termiosSettings.controlFlags.CSTOPB("CSTOPB"),
            impl_->termiosSettings.controlFlags.CREAD("CREAD"),
            impl_->termiosSettings.controlFlags.PARENB("PARENB"),
            impl_->termiosSettings.controlFlags.PARODD("PARODD"),
            impl_->termiosSettings.controlFlags.HUPCL("HUPCL"),
            impl_->termiosSettings.controlFlags.CLOCAL("CLOCAL"),
            impl_->termiosSettings.controlFlags.LOBLK("LOBLK"),
            impl_->termiosSettings.controlFlags.CIBAUD("CIBAUD"),
            impl_->termiosSettings.controlFlags.CMSPAR("CMSPAR"),
            impl_->termiosSettings.controlFlags.CRTSCTS("CRTSCTS")
        )),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "localFlagsSubgroupTitle")),
        subgroup({}, fragment(
            impl_->termiosSettings.localFlags.ISIG("ISIG"),
            impl_->termiosSettings.localFlags.ICANON("ICANON"),
            impl_->termiosSettings.localFlags.XCASE("XCASE"),
            impl_->termiosSettings.localFlags.ECHO("ECHO"),
            impl_->termiosSettings.localFlags.ECHOE("ECHOE"),
            impl_->termiosSettings.localFlags.ECHOK("ECHOK"),
            impl_->termiosSettings.localFlags.ECHONL("ECHONL"),
            impl_->termiosSettings.localFlags.ECHOPRT("ECHOPRT"),
            impl_->termiosSettings.localFlags.ECHOKE("ECHOKE"),
            impl_->termiosSettings.localFlags.FLUSHO("FLUSHO"),
            impl_->termiosSettings.localFlags.NOFLSH("NOFLSH"),
            impl_->termiosSettings.localFlags.TOSTOP("TOSTOP"),
            impl_->termiosSettings.localFlags.PENDIN("PENDIN"),
            impl_->termiosSettings.localFlags.IEXTEN("IEXTEN")
        )),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "ccSettingsSubgroupTitle")),
        subgroup({
            .engagedStatus = &impl_->termiosSettings.ccEngaged,
            .groupTitle = language->getObserved("settings", "ccSettingsSubgroupTitle")
        }, fragment(
            impl_->termiosSettings.cc.VDISCARD("VDISCARD"),
            impl_->termiosSettings.cc.VDSUSP("VDSUSP"),
            impl_->termiosSettings.cc.VEOF("VEOF"),
            impl_->termiosSettings.cc.VEOL("VEOL"),
            impl_->termiosSettings.cc.VEOL2("VEOL2"),
            impl_->termiosSettings.cc.VERASE("VERASE"),
            impl_->termiosSettings.cc.VINTR("VINTR"),
            impl_->termiosSettings.cc.VKILL("VKILL"),
            impl_->termiosSettings.cc.VLNEXT("VLNEXT"),
            impl_->termiosSettings.cc.VMIN("VMIN"),
            impl_->termiosSettings.cc.VQUIT("VQUIT"),
            impl_->termiosSettings.cc.VREPRINT("VREPRINT"),
            impl_->termiosSettings.cc.VSTART("VSTART"),
            impl_->termiosSettings.cc.VSTATUS("VSTATUS"),
            impl_->termiosSettings.cc.VSTOP("VSTOP"),
            impl_->termiosSettings.cc.VSUSP("VSUSP"),
            impl_->termiosSettings.cc.VSWTCH("VSWTCH"),
            impl_->termiosSettings.cc.VTIME("VTIME"),
            impl_->termiosSettings.cc.VWERASE("VWERASE")
        )),
        impl_->termiosSettings.iSpeed(language->getObserved("settings", "termios", "iSpeedHelpText")),
        impl_->termiosSettings.oSpeed(language->getObserved("settings", "termios", "oSpeedHelpText"))
    );
    // clang-format on

    // clang-format off
    return fragment(
        ui5::message_strip{
            "design"_prop = "Information",
            "hideCloseButton"_prop = true,
        }(
            language->getObserved("settings", "inheritableSettingsInfoMessage")
        ),
        group({
            .isCollapsed = impl_->collapsibleStates.sshOptions,
            .content = std::move(sshOptions),
            .headerTitle = language->getObserved("settings", "sshOptionsGroupName"),
            .currentGroupKey = &impl_->sshOptions.groupKey,
            .groupKeys = &impl_->sshOptions.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheritable
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.sftpOptions,
            .content = std::move(sftpOptions),
            .headerTitle = language->getObserved("settings", "sftpOptionsGroupName"),
            .currentGroupKey = &impl_->sftpOptions.groupKey,
            .groupKeys = &impl_->sftpOptions.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheritable
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.terminalOptions,
            .content = std::move(terminalOptions),
            .headerTitle = language->getObserved("settings", "terminalOptionsGroupName"),
            .currentGroupKey = &impl_->terminalOptions.groupKey,
            .groupKeys = &impl_->terminalOptions.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheritable
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.queueOptions,
            .content = std::move(queueOptions),
            .headerTitle = language->getObserved("settings", "queueOptionsGroupName"),
            .currentGroupKey = &impl_->queueOptions.groupKey,
            .groupKeys = &impl_->queueOptions.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheritable
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.termios,
            .content = std::move(termios),
            .headerTitle = language->getObserved("settings", "termiosGroupName"),
            .currentGroupKey = &impl_->termiosSettings.groupKey,
            .groupKeys = &impl_->termiosSettings.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheritable
        })
    );
    // clang-format on
}

Nui::ElementRenderer Settings::currentSession()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    auto overarchingSettings = fragment(
        impl_->currentSessionOptions.terminalEngineType(
            language->getObserved("settings", "sessionOptions", "terminalEngineType")
        ),
        impl_->currentSessionOptions.icon(
            language->getObserved("settings", "sessionOptions", "icon")
        ),
        impl_->currentSessionOptions.orderBy(
            language->getObserved("settings", "sessionOptions", "orderBy")
        ),
        impl_->currentSessionOptions.isStartupSession(
            language->getObserved("settings", "sessionOptions", "isStartupSession")
        ),
        div{
            style = observe(impl_->currentSessionOptions.terminalEngineType.state()).generate([](Persistence::TerminalEngineType type) {
                return type == Persistence::TerminalEngineType::ssh ? "" : "display: none;";
            }),
        }(
            h1{
                class_ = "settings-header"
            }(language->getObserved("settings", "sessionOptions", "sshSessionServerOptions")),
            subgroup({},
                fragment(
                    impl_->currentSessionOptions.sshSessionOptions.host(language->getObserved("settings", "sessionOptions", "host")),
                    impl_->currentSessionOptions.sshSessionOptions.port(language->getObserved("settings", "sessionOptions", "port")),
                    impl_->currentSessionOptions.sshSessionOptions.user(language->getObserved("settings", "sessionOptions", "user")),
                    impl_->currentSessionOptions.sshSessionOptions.sshKey(language->getObserved("settings", "sessionOptions", "sshKey")),
                    impl_->currentSessionOptions.sshSessionOptions.openSftpByDefault(language->getObserved("settings", "sessionOptions", "openSftpByDefault"))
                )
            )
        ),
        div{
            style = observe(impl_->currentSessionOptions.terminalEngineType.state()).generate([](Persistence::TerminalEngineType type) {
                return type == Persistence::TerminalEngineType::shell ? "" : "display: none;";
            }),
        }(
            h1{
                class_ = "settings-header"
            }(language->getObserved("settings", "sessionOptions", "localSessionOptions")),
            subgroup({},
                fragment(
                    impl_->currentSessionOptions.executingSessionOptions.isPty(language->getObserved("settings", "sessionOptions", "isPty")),
                    impl_->currentSessionOptions.executingSessionOptions.command(language->getObserved("settings", "sessionOptions", "command")),
                    impl_->currentSessionOptions.executingSessionOptions.arguments(language->getObserved("settings", "sessionOptions", "arguments")),
                    impl_->currentSessionOptions.executingSessionOptions.environment(
                        language->getObserved("settings", "sessionOptions", "environmentVariables")),
                    impl_->currentSessionOptions.executingSessionOptions.exitTimeoutSeconds(
                        language->getObserved("settings", "sessionOptions", "exitTimeoutSeconds")),
                    impl_->currentSessionOptions.executingSessionOptions.cleanEnvironment(
                        language->getObserved("settings", "sessionOptions", "cleanEnvironment"))
                )
            )
        )
    );

    auto& sshOptionStruct = impl_->currentSessionOptions.sshSessionOptions.sshOptions;
    auto sshOptions = fragment(
        sshOptionStruct.sshDirectory(language->getObserved("settings", "sshOptions", "sshDirectory")),
        sshOptionStruct.knownHostsFile(language->getObserved("settings", "sshOptions", "knownHostsFile")),
        sshOptionStruct.tryAgentForAuthentication(language->getObserved("settings", "sshOptions", "tryAgentForAuthentication")),
        sshOptionStruct.usePublicKeyAutoAuth(language->getObserved("settings", "sshOptions", "usePublicKeyAutoAuth")),
        sshOptionStruct.logVerbosity(language->getObserved("settings", "sshOptions", "logVerbosity")),
        sshOptionStruct.keyExchangeAlgorithms(language->getObserved("settings", "sshOptions", "keyExchangeAlgorithms")),
        sshOptionStruct.compressionClientToServer(language->getObserved("settings", "sshOptions", "compressionClientToServer")),
        sshOptionStruct.compressionServerToClient(language->getObserved("settings", "sshOptions", "compressionServerToClient")),
        sshOptionStruct.compressionLevel(language->getObserved("settings", "sshOptions", "compressionLevel")),
        sshOptionStruct.strictHostKeyCheck(language->getObserved("settings", "sshOptions", "strictHostKeyCheck")),
        sshOptionStruct.proxyCommand(language->getObserved("settings", "sshOptions", "proxyCommand")),
        sshOptionStruct.gssapiServerIdentity(language->getObserved("settings", "sshOptions", "gssapiServerIdentity")),
        sshOptionStruct.gssapiClientIdentity(language->getObserved("settings", "sshOptions", "gssapiClientIdentity")),
        sshOptionStruct.gssapiDelegateCredentials(language->getObserved("settings", "sshOptions", "gssapiDelegateCredentials")),
        sshOptionStruct.noDelay(language->getObserved("settings", "sshOptions", "noDelay")),
        sshOptionStruct.bypassConfig(language->getObserved("settings", "sshOptions", "bypassConfig")),
        sshOptionStruct.identityAgent(language->getObserved("settings", "sshOptions", "identityAgent")),
        sshOptionStruct.connectTimeoutSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutSeconds")),
        sshOptionStruct.connectTimeoutUSeconds(language->getObserved("settings", "sshOptions", "connectTimeoutUSeconds"))
    );

    return fragment(
        ui5::message_strip{
            "design"_prop = "Information",
            "hideCloseButton"_prop = true,
        }(
            language->getObserved("settings", "inheritableSettingsInfoMessage")
        ),
        group({
            .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.overarchingSettings,
            .content = std::move(overarchingSettings),
            .headerTitle = language->getObserved("settings", "sessionOptions", "basicSettings")
        }),
        group({
            .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.sshOptions,
            .content = std::move(sshOptions),
            .headerTitle = language->getObserved("settings", "sshOptionsGroupName"),
            .currentGroupKey = &impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKey,
            .groupKeys = &impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKeys,
            .inheritanceBehavior = GroupParameters::InheritanceBehavior::Inheriting,
            .engineTypeFilter = &impl_->currentSessionOptions.terminalEngineType.state(),
            .engineTypeFilterValue = Persistence::TerminalEngineType::ssh
        })
    );
    // clang-format on
}

Nui::ElementRenderer Settings::subgroup(SubgroupParameters&& params, Nui::ElementRenderer content)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    // clang-format off
    return div{
        class_ = "settings-subgroup",
    }(
        div{
            class_ = "settings-subgroup-header",
            style = params.engagedStatus ? "border: 1px solid var(--sapContent_ForegroundBorderColor); background-color: var(--darkerBackground)" : "",
            onClick = [engagedStatus = params.engagedStatus]() {
                if (engagedStatus)
                    *engagedStatus = !*engagedStatus;
            }
        }(
            // switch to enable/disable entire subgroup:
            [this, engagedStatus = params.engagedStatus, title = std::move(params.groupTitle)]() mutable -> Nui::ElementRenderer {
                if (!engagedStatus || !title)
                    return Nui::nil();

                return fragment(
                    ui5::label{
                        "design"_prop = "Bold",
                    }(std::move(title).value()),
                    ui5::switch_{
                        "checked"_prop = engagedStatus->value(), // initial not observed
                        "change"_event = [this, engagedStatus](Nui::val event) {
                            *engagedStatus = event["target"]["checked"].as<bool>();
                            onChange();
                        },
                    }()
                );
            }()
        ),
        div{
            class_ = "settings-subgroup-content",
            style = params.engagedStatus ? "margin-top: 20px; padding-top: 32px;" : ""
        }(
            std::move(content)
        )
    );
    // clang-format on
}

Nui::ElementRenderer Settings::group(GroupParameters&& params)
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    auto groupKeyContainer = [&params, this]() -> Nui::ElementRenderer
    {
        if (!params.currentGroupKey)
            return Nui::nil();

        auto addGroupButton = [&params, this]() -> Nui::ElementRenderer
        {
            if (params.inheritanceBehavior != GroupParameters::InheritanceBehavior::Inheritable)
                return Nui::nil();

            return ui5::button{
                "design"_prop = "Primary",
                "icon"_prop = "add",
                "click"_event = [this, currentGroupKey = params.currentGroupKey, groupKeys = params.groupKeys]()
                {
                    impl_->inputDialog->open({
                        .whatFor = language->get("settings", "groupKey"),
                        .prompt = language->get("settings", "enterGroupKeyPlaceholder"),
                        .headerText = language->get("settings", "groupKey"),
                        .isPassword = false,
                        .onConfirm = [this, currentGroupKey, groupKeys](std::optional<std::string> const& result)
                        {
                            if (result && !result->empty())
                            {
                                *currentGroupKey = *result;
                                if (std::find((*groupKeys)->begin(), (*groupKeys)->end(), *result) ==
                                    (*groupKeys)->end())
                                {
                                    (*groupKeys)->push_back(*result);
                                    groupKeys->modify();
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                }
                                else
                                {
                                    // Key already exists, do nothing or show a message if needed
                                    impl_->confirmDialog->open({
                                        .state = ConfirmDialog::State::Critical,
                                        .headerText = language->get("settings", "groupKeyExistsHeader"),
                                        .text = language->get("settings", "groupKeyExistsText"),
                                        .buttons = ConfirmDialog::Button::Ok,
                                    });
                                }
                            }
                        },
                    });
                },
            }();
        };

        auto removeGroupButton = [&params, this]() -> Nui::ElementRenderer
        {
            if (params.inheritanceBehavior != GroupParameters::InheritanceBehavior::Inheritable)
                return Nui::nil();

            return ui5::button{
                "design"_prop = "Negative",
                "icon"_prop = "delete",
                "click"_event = [this, currentGroupKey = params.currentGroupKey, groupKeys = params.groupKeys]()
                {
                    if (!**currentGroupKey)
                        return;

                    impl_->confirmDialog->open({
                        .state = ConfirmDialog::State::Critical,
                        .headerText = language->get("settings", "confirmDeleteGroupKeyHeader"),
                        .text = language->get("settings", "confirmDeleteGroupKeyText"),
                        .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                        .onClose = [currentGroupKey, groupKeys](ConfirmDialog::Button btn)
                        {
                            if (!**currentGroupKey)
                                return;

                            const auto key = ***currentGroupKey;

                            if (btn == ConfirmDialog::Button::Ok)
                            {
                                groupKeys->erase(
                                    std::remove((*groupKeys)->begin(), (*groupKeys)->end(), key), (*groupKeys)->end()
                                );
                                groupKeys->modify();
                                if (!(*groupKeys)->empty())
                                {
                                    *currentGroupKey = ((*groupKeys)->front());
                                }
                                else
                                {
                                    *currentGroupKey = "default"s;
                                    (*groupKeys)->push_back("default"s);
                                    groupKeys->modify();
                                }
                                Nui::globalEventContext.executeActiveEventsImmediately();
                            }
                        },
                    });
                },
            }();
        };

        return div{class_ = "settings-group-key-container"}(
            ui5::label{
                "design"_prop = "Bold",
            }(language->getObserved("settings", "groupKey")),
            ui5::select{
                "change"_event =
                    [this,
                        currentGroupKey = params.currentGroupKey,
                        inheritanceBehavior = params.inheritanceBehavior](Nui::val event)
                {
                    *currentGroupKey = event["detail"]["selectedOption"]["leKey"].as<std::string>();
                    if (inheritanceBehavior == GroupParameters::InheritanceBehavior::Inheritable)
                        reloadInheritables();
                    else if (inheritanceBehavior == GroupParameters::InheritanceBehavior::Inheriting)
                        reloadInheritance();
                },
                "value"_prop = *params.currentGroupKey,
            }(params.groupKeys->map(
                [](long long, std::string const& inheritKey) -> Nui::ElementRenderer
                {
                    return ui5::option{
                        "leKey"_prop = inheritKey,
                    }(inheritKey);
                }
            )),
            addGroupButton(),
            removeGroupButton()
        );
    };

    auto makeSessionTypeFilteredDiv =
        [engineTypeFilter = params.engineTypeFilter, engineTypeFilterValue = params.engineTypeFilterValue]()
    {
        if (engineTypeFilter)
        {
            return div{
                class_ = "settings-group",
                style = observe(*engineTypeFilter)
                    .generate(
                        [engineTypeFilterValue](Persistence::TerminalEngineType type)
                        {
                            return type == engineTypeFilterValue ? "" : "display: none;";
                        }
                    ),
            };
        }
        return div{
            class_ = "settings-group",
        };
    };

    // clang-format off
    return makeSessionTypeFilteredDiv()(
        div{
            class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                return classes("settings-group-header", isCollapsed ? "collapsed" : "uncollapsed");
            }),
            onClick = [&isCollapsed = params.isCollapsed](){
                isCollapsed = !*isCollapsed;
            }
        }(
            // collapse indicator:
            span{class_ = "settings-group-header-collapse-indicator"}(
                ui5::icon{
                    "name"_prop = observe(params.isCollapsed).generate([](bool isCollapsed) {
                        return isCollapsed ? "navigation-right-arrow" : "navigation-down-arrow";
                    }),
                    "design"_prop = "Neutral",
                    style = "color: var(--sapTextColor)"
                }()
            ),
            span{class_ = "settings-group-header-title"}(std::move(params.headerTitle)),
            [&params]() -> Nui::ElementRenderer {
                if (params.isEnabled) {
                    return ui5::switch_{
                        "design"_prop = "Emphasized",
                        "state"_prop = *params.isEnabled,
                        "change"_event = [&params](Nui::val event) {
                            *params.isEnabled = event["state"].as<bool>();
                        },
                    }();
                }
                return Nui::nil();
            }()
        ),
        div{
            class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
                return classes("settings-group-content", isCollapsed ? "collapsed" : "uncollapsed");
            }),
            style = Nui::Attributes::Style{
                "padding-top"_style = params.currentGroupKey ? "0px" : "8px"
            },
        }(
            groupKeyContainer(),
            std::move(params.content)
        )
    );
    // clang-format on
}