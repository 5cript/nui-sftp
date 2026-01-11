#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/dialog/new_session_dialog.hpp>
#include <frontend/classes.hpp>
#include <frontend/settings/combo_setting.hpp>
#include <frontend/settings/text_setting.hpp>
#include <frontend/state_holder_with_dialog.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <ui5/components/button.hpp>
#include <ui5/components/switch.hpp>
#include <ui5/components/busy_indicator.hpp>

#include <nui/frontend/api/throttle.hpp>
#include <nui/frontend/api/timer.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

struct GeneralSettings
{
    struct CollapsibleStates
    {
        Nui::Observed<bool> localization{false};
        Nui::Observed<bool> loggingAndErrorReporting{false};
    } collapsibleStates;

    struct LogLevel
    {
        Nui::Observed<Log::Level> logLevel;
        ComboSetting<Log::Level, std::string> comboSetting;
    } logLevel;

    struct Localization
    {
        Nui::Observed<std::string> languageCode{};
        Nui::Observed<std::string> dateTimeFormatString{};

        ComboSetting<std::string, std::string> language;
        TextSetting dateTimeFormat;
    } localization;

    GeneralSettings(std::invocable auto const& onChange, FrontendEvents* events)
        : logLevel{
              .logLevel{Persistence::State{}.logLevel},
              .comboSetting = {
                  logLevel.logLevel,
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
                  [this]()
                  {
                      logLevel.logLevel = Persistence::State{}.logLevel;
                  },
                  [](Log::Level const& level)
                  {
                      return Utility::enumToString<Log::Level>(level);
                  },
                  [](Log::Level const& level) -> std::optional<std::string>
                  {
                      switch (level)
                      {
                          case Log::Level::Trace:
                              return "activity-items";
                          case Log::Level::Debug:
                              return "zoom-in";
                          case Log::Level::Info:
                              return "information";
                          case Log::Level::Warning:
                              return "alert";
                          case Log::Level::Error:
                              return "error";
                          case Log::Level::Critical:
                              return "incident";
                          case Log::Level::Off:
                              return "hide";
                          default:
                              return std::nullopt;
                      }
                  }
              }
          }
        , localization{
              .language = {
                  localization.languageCode,
                  {"en_US", "de_DE"},
                  language->getObserved("settings", "general", "localization", "languageHelpText"),
                  [onChange, this, events]()
                  {
                      onChange();
                      events->onLanguageChanged = localization.languageCode.value();
                      events->onLanguageChanged.modifyNow();
                  },
                  [this, events]()
                  {
                      localization.languageCode = Persistence::State{}.localizationOptions.languageCode;
                      events->onLanguageChanged = localization.languageCode.value();
                      events->onLanguageChanged.modifyNow();
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
              .dateTimeFormat = TextSetting{
                  localization.dateTimeFormatString,
                  language->getObserved("settings", "general", "localization", "dateTimeFormatHelpText"),
                  onChange,
                  [this]()
                  {
                      localization.dateTimeFormatString = Persistence::State{}.localizationOptions.dateTimeFormatString;
                  },
              },
          }
    {}
};

struct Settings::Implementation
{
    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::ThrottledFunction throttledSave{};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};
    Nui::Observed<bool> saveInProgress{false};

    Nui::Observed<std::vector<Settings::SectionSelectorOptions>> sessionSelectors{{
        {.sessionId = "Session 1", .icon = "it-system"},
        {.sessionId = "Session 2", .icon = "it-system"},
        {.sessionId = "Session 3", .icon = "it-system"},
    }};

    GeneralSettings generalSettings;

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

void Settings::applySettingsToState(Persistence::State& state)
{
    state.logLevel = *impl_->generalSettings.logLevel.logLevel;
    state.localizationOptions.languageCode = *impl_->generalSettings.localization.languageCode;
    state.localizationOptions.dateTimeFormatString = *impl_->generalSettings.localization.dateTimeFormatString;
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

            impl_->generalSettings.logLevel.logLevel = impl_->stateHolder->stateCache().logLevel;
            impl_->generalSettings.localization.languageCode =
                impl_->stateHolder->stateCache().localizationOptions.languageCode;
            impl_->generalSettings.localization.dateTimeFormatString =
                impl_->stateHolder->stateCache().localizationOptions.dateTimeFormatString;

            Nui::globalEventContext.executeActiveEventsImmediately();
        }
    );
}

ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Settings);

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
        class_ = "settings-page-sections",
    }(
        observe(impl_->activeSection).generate(
            [this](Section activeSection) -> Nui::ElementRenderer {
                switch (activeSection) {
                    case Section::GeneralSettings:
                        return generalSettings();
                    case Section::GlobalInheritables:
                        return Nui::nil(); // TODO
                    case Section::Session:
                        return Nui::nil(); // TODO
                    default:
                        return Nui::nil(); // TODO
                }
                return Nui::nil();
            }
        )
    );
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
        [](auto const& result)
        {
            Log::info("New session created: {} with icon {}", result.sessionName, result.iconName);
        }
    );
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

            if (options.thisSection == Section::Session && options.sessionId.has_value()) {
                impl_->activeSection = Section::Session;
                impl_->activeSession = options.sessionId;
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
        impl_->generalSettings.logLevel.comboSetting(language->getObserved("settings", "logLevel"))
    );

    auto localization = fragment(
        impl_->generalSettings.localization.language(language->getObserved("language")),
        impl_->generalSettings.localization.dateTimeFormat(language->getObserved("settings", "dateTimeFormatString"))
    );
    // clang-format on

    // clang-format off
    return fragment(
        group({
            .isCollapsed = impl_->generalSettings.collapsibleStates.localization,
            .content = std::move(localization),
            .headerTitle = language->getObserved("settings", "generalSettings")
        }),
        group({
            .isCollapsed = impl_->generalSettings.collapsibleStates.loggingAndErrorReporting,
            .content = std::move(loggingAndErrorReporting),
            .headerTitle = language->getObserved("settings", "loggingAndErrorReportingGroupHeader")
        })
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

    // clang-format off
    return div{
        class_ = "settings-group",
    }(
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
        div{class_ = observe(params.isCollapsed).generate([](bool isCollapsed) {
            return classes("settings-group-content", isCollapsed ? "collapsed" : "uncollapsed");
        })}(
            std::move(params.content)
        )
    );
    // clang-format on
}