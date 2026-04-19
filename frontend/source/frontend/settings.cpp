#include <frontend/settings.hpp>

#include <frontend/components/icon_panel.hpp>
#include <frontend/icon_from_name.hpp>
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
#include <frontend/settings/log_options.hpp>
#include <frontend/settings/atomic_setting/combo_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/map_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>
#include <frontend/settings/atomic_setting/color_setting.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/subgroup.hpp>
#include <frontend/settings/setting_group.hpp>
#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/button.hpp>
#include <script-nui-components/switch.hpp>
#include <script-nui-components/select.hpp>
#include <script-nui-components/message_strip.hpp>

#include <frontend/svgs/decline.hpp>
#include <frontend/svgs/settings.hpp>
#include <frontend/svgs/it-system.hpp>
#include <frontend/svgs/delete.hpp>
#include <frontend/svgs/add.hpp>
#include <frontend/svgs/wrench.hpp>
#include <frontend/svgs/navigation-right-arrow.hpp>
#include <frontend/svgs/navigation-down-arrow.hpp>
#include <frontend/svgs/action-settings.hpp>

#include <nui/frontend/api/throttle.hpp>
#include <nui/frontend/val.hpp>
#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

using namespace std::string_literals;
namespace Snc = ScriptNuiComponents;

struct Settings::Implementation
{
    struct CollapsibleStates
    {
        Nui::Observed<bool> sshOptions{true};
        Nui::Observed<bool> sftpOptions{true};
        Nui::Observed<bool> termios{true};
        Nui::Observed<bool> terminalOptions{true};
        Nui::Observed<bool> queueOptions{true};

        struct SessionCollapsibles
        {
            Nui::Observed<bool> overarchingSettings{true};
            Nui::Observed<bool> sshOptions{true};
            Nui::Observed<bool> sftpOptions{true};
            Nui::Observed<bool> terminalOptions{true};
            Nui::Observed<bool> termios{true};
            Nui::Observed<bool> queueOptions{true};
        } sessionCollapsibles{};
    } collapsibleStates{};

    Persistence::StateHolder* stateHolder;
    FrontendEvents* events;
    InputDialog* inputDialog;
    ConfirmDialog* confirmDialog;
    MultiInputDialog* multiInputDialog;
    NewSessionDialog newSessionDialog{"settings"};
    Nui::ThrottledFunction throttledSave{};
    Nui::ThrottledFunction throttledReloadInheritance{};
    Nui::Observed<Settings::Section> activeSection{Settings::Section::GeneralSettings};
    Nui::Observed<std::optional<std::string>> activeSession{};
    Nui::Observed<bool> saveInProgress{false};
    bool applyingToUi{false};

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

    Nui::Observed<bool> wasInitiallyLoaded{false};
    Nui::Observed<bool> initialLoadDone{false};

    // Tracks whether the user has modified any setting that only takes effect
    // after an app restart. The baseline is captured each time the settings
    // dialog opens; listeners on the critical fields flip requiresRestart
    // whenever the current value diverges from the baseline (and back to
    // false if the user reverts).
    struct RestartCriticalBaseline
    {
        std::string languageCode;
        std::string logDirectory;
        bool disableFileLogging{false};
        std::optional<std::filesystem::path> temporaryDownloadsDirectory;
        std::optional<int> concurrency;
    };
    Nui::Observed<bool> requiresRestart{false};
    RestartCriticalBaseline restartBaseline{};

    Nui::ListenRemover<decltype(FrontendEvents::settingsOpen)> settingsOpenListener{};
    Nui::ListenRemover<decltype(FrontendEvents::requestedSettingScrollId)> requestedSettingScrollIdListener{};

    void captureRestartBaseline()
    {
        restartBaseline = RestartCriticalBaseline{
            .languageCode = generalSettings.localization.language.value(),
            .logDirectory = generalSettings.logOptions.logDirectory.value(),
            .disableFileLogging = generalSettings.logOptions.disableFileLogging.value(),
            .temporaryDownloadsDirectory =
                generalSettings.localFilesystemOptions.temporaryDownloadsDirectory.value(),
            .concurrency = sftpOptions.concurrency.value(),
        };
        requiresRestart = false;
    }

    void recomputeRequiresRestart()
    {
        if (applyingToUi)
            return;
        requiresRestart =
            generalSettings.localization.language.value() != restartBaseline.languageCode
            || generalSettings.logOptions.logDirectory.value() != restartBaseline.logDirectory
            || generalSettings.logOptions.disableFileLogging.value() != restartBaseline.disableFileLogging
            || generalSettings.localFilesystemOptions.temporaryDownloadsDirectory.value()
                != restartBaseline.temporaryDownloadsDirectory
            || sftpOptions.concurrency.value() != restartBaseline.concurrency;
    }

    Implementation(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::function<std::optional<nlohmann::json>()> const& obtainCurrentLayout,
        InputDialog& inputDialog,
        ConfirmDialog& confirmDialog,
        MultiInputDialog& multiInputDialog,
        std::invocable auto const& onChange,
        std::invocable auto const& reloadInheritance
    )
        : stateHolder{stateHolder}
        , events{events}
        , inputDialog{&inputDialog}
        , confirmDialog{&confirmDialog}
        , multiInputDialog{&multiInputDialog}
        , generalSettings{onChange, events, inputDialog, multiInputDialog}
        , termiosSettings{[onChange, reloadInheritance]()
              {
                  onChange();
                  reloadInheritance();
              }}
        , sshOptions{[onChange, reloadInheritance]()
              {
                  onChange();
                  reloadInheritance();
              }, inputDialog, multiInputDialog}
        , sftpOptions{[onChange, reloadInheritance]()
              {
                  onChange();
                  reloadInheritance();
              }}
        , terminalOptions{[onChange, reloadInheritance]()
              {
                  onChange();
                  reloadInheritance();
              }}
        , queueOptions{[onChange, reloadInheritance]()
              {
                  onChange();
                  reloadInheritance();
              }}
        , currentSessionOptions{onChange, obtainCurrentLayout, confirmDialog, inputDialog, multiInputDialog}
    {}
};

Settings::Settings(
    Persistence::StateHolder* stateHolder,
    FrontendEvents* events,
    std::function<std::optional<nlohmann::json>()> const& obtainCurrentLayout,
    InputDialog& inputDialog,
    ConfirmDialog& confirmDialog,
    MultiInputDialog& multiInputDialog
)
    : impl_{std::make_unique<Implementation>(
          stateHolder,
          events,
          obtainCurrentLayout,
          inputDialog,
          confirmDialog,
          multiInputDialog,
          [this]()
          {
              if (impl_->applyingToUi)
                  return;
              if (impl_->throttledSave.valid())
                  impl_->throttledSave();
              impl_->recomputeRequiresRestart();
          },
          [this]()
          {
              if (impl_->throttledReloadInheritance.valid())
                  impl_->throttledReloadInheritance();
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

    Nui::throttle(
        500,
        [this]()
        {
            reloadInheritance();
        },
        [this](Nui::ThrottledFunction&& func)
        {
            impl_->throttledReloadInheritance = std::move(func);
        },
        true
    );

    impl_->settingsOpenListener = Nui::smartListen(
        impl_->events->settingsOpen,
        [this](bool open)
        {
            if (open)
            {
                applySettingsToUi();
                if (!*impl_->wasInitiallyLoaded)
                {
                    // Three-pass reveal:
                    //  Frame 1: loader-only tree paints.
                    //  Frame 2: mount heavy sections() subtree (still covered by loader).
                    //  Frame 3: hide loader — content is already laid out underneath.
                    // rAF callbacks receive a DOMHighResTimeStamp; the Nui::bind
                    // functor must accept it or emscripten raises a BindingError
                    // (void0ArgsFunctor called with 1 argument).
                    auto raf = Nui::val::global("requestAnimationFrame");
                    raf(Nui::bind(
                        [raf, this](Nui::val) {
                            raf(Nui::bind(
                                [raf, this](Nui::val) {
                                    impl_->wasInitiallyLoaded = true;
                                    Nui::globalEventContext.executeActiveEventsImmediately();
                                    raf(Nui::bind(
                                        [this](Nui::val) {
                                            impl_->initialLoadDone = true;
                                            Nui::globalEventContext.executeActiveEventsImmediately();
                                        },
                                        std::placeholders::_1
                                    ));
                                },
                                std::placeholders::_1
                            ));
                        },
                        std::placeholders::_1
                    ));
                }
            }
        }
    );

    // Reacts to requestOpenSettingsAtId: poll until the target element is
    // mounted (settings render is multi-frame on first open), walk up to its
    // [data-settings-section] ancestor to decide which Section to activate,
    // then scroll into view and briefly highlight.
    impl_->requestedSettingScrollIdListener = Nui::smartListen(
        impl_->events->requestedSettingScrollId,
        [this](std::optional<std::string> const& htmlId)
        {
            if (!htmlId || htmlId->empty())
                return;

            // Consume the request immediately so re-entry via nullopt doesn't
            // recurse and a follow-up request with the same id still fires.
            const std::string idCopy = *htmlId;
            impl_->events->requestedSettingScrollId = std::nullopt;

            // Polls up to ~30 frames (~500ms @ 60Hz) for the element to mount.
            // Settings rendering takes up to 3 rAF on first open; this comfortably
            // absorbs that without arbitrary sleeps.
            auto attempts = std::make_shared<int>(0);
            static constexpr int maxAttempts = 30;
            auto tryLocate = std::make_shared<std::function<void()>>();
            *tryLocate = [this, idCopy, attempts, tryLocate]() {
                auto document = Nui::val::global("document");
                auto element = document.call<Nui::val>("getElementById", idCopy);
                if (element.isNull() || element.isUndefined())
                {
                    if (++(*attempts) >= maxAttempts)
                    {
                        Log::warn(
                            "requestedSettingScrollId: no element with id '{}' after {} frames",
                            idCopy, maxAttempts
                        );
                        return;
                    }
                    Nui::val::global("requestAnimationFrame")(
                        Nui::bind([tryLocate](Nui::val) { (*tryLocate)(); }, std::placeholders::_1)
                    );
                    return;
                }

                auto section = element.call<Nui::val>("closest", std::string{"[data-settings-section]"});
                if (section.isNull() || section.isUndefined())
                {
                    Log::warn(
                        "requestedSettingScrollId: element '{}' has no [data-settings-section] ancestor",
                        idCopy
                    );
                    return;
                }

                const auto sectionName = section["dataset"]["settingsSection"].as<std::string>();
                if (sectionName == "GeneralSettings")
                    impl_->activeSection = Section::GeneralSettings;
                else if (sectionName == "GlobalInheritables")
                    impl_->activeSection = Section::GlobalInheritables;
                else if (sectionName == "Session")
                    impl_->activeSection = Section::Session;
                else
                {
                    Log::warn("requestedSettingScrollId: unknown section '{}'", sectionName);
                    return;
                }
                Nui::globalEventContext.executeActiveEventsImmediately();

                // Expand any collapsed settings-group ancestors so the target
                // isn't hidden behind a max-height:0 group.
                Nui::val::global("addressableSettings")
                    .call<void>("expandCollapsedGroupsContaining", idCopy);

                // rAF twice so the section display-swap + any group expansion
                // have painted before scroll measurement.
                auto raf = Nui::val::global("requestAnimationFrame");
                raf(Nui::bind([raf, idCopy](Nui::val) {
                    raf(Nui::bind([idCopy](Nui::val) {
                        Nui::val::global("addressableSettings")
                            .call<void>("scrollToAndHighlight", idCopy);
                    }, std::placeholders::_1));
                }, std::placeholders::_1));
            };
            (*tryLocate)();
        }
    );
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Settings);

void Settings::applySettingsToState(Persistence::State& state)
{
    // Uncategorized / General:
    impl_->generalSettings.applyToState(state);

    auto synchronizeGroupKeys = [](auto& map, auto const& groupKeys)
    {
        // Remove entries not in groupKeys
        for (auto it = map.begin(); it != map.end();)
        {
            if (std::find(groupKeys.begin(), groupKeys.end(), it->first) == groupKeys.end())
                it = map.erase(it);
            else
                ++it;
        }
    };

    // Termios
    if (impl_->termiosSettings.groupKey.value())
    {
        Persistence::Termios termiosEntry{};
        impl_->termiosSettings.applyToState(termiosEntry);
        synchronizeGroupKeys(state.termios, impl_->termiosSettings.groupKeys.value());
        state.termios[*impl_->termiosSettings.groupKey.value()] = std::move(termiosEntry);
    }

    // SSH Options:
    if (impl_->sshOptions.groupKey.value())
    {
        Persistence::SshOptions sshOptionsEntry{};
        impl_->sshOptions.applyToState(sshOptionsEntry);
        synchronizeGroupKeys(state.sshOptions, impl_->sshOptions.groupKeys.value());
        state.sshOptions[*impl_->sshOptions.groupKey.value()] = std::move(sshOptionsEntry);
    }

    // SftpOptions:
    if (impl_->sftpOptions.groupKey.value())
    {
        Persistence::SftpOptions sftpOptionsEntry{};
        impl_->sftpOptions.applyToState(sftpOptionsEntry);
        synchronizeGroupKeys(state.sftpOptions, impl_->sftpOptions.groupKeys.value());
        state.sftpOptions[*impl_->sftpOptions.groupKey.value()] = std::move(sftpOptionsEntry);
    }

    // Terminal Options
    if (impl_->terminalOptions.groupKey.value())
    {
        Persistence::TerminalOptions terminalOptionsEntry{};
        impl_->terminalOptions.applyToState(terminalOptionsEntry);
        synchronizeGroupKeys(state.terminalOptions, impl_->terminalOptions.groupKeys.value());
        state.terminalOptions[*impl_->terminalOptions.groupKey.value()] = std::move(terminalOptionsEntry);
    }

    // Queue Options:
    if (impl_->queueOptions.groupKey.value())
    {
        Persistence::QueueOptions queueOptionsEntry{};
        impl_->queueOptions.applyToState(queueOptionsEntry);
        synchronizeGroupKeys(state.queueOptions, impl_->queueOptions.groupKeys.value());
        state.queueOptions[*impl_->queueOptions.groupKey.value()] = std::move(queueOptionsEntry);
    }

    // Session Options:
    applySessionOptionsToState();
}

void Settings::applySettingsToUi()
{
    impl_->events->onReloadThemes.modifyNow();
    loadState(
        *impl_->stateHolder,
        impl_->confirmDialog,
        [this](bool success, Persistence::State const&)
        {
            if (!success)
                return;

            impl_->applyingToUi = true;
            Nui::ScopeExit clearApplyingFlag{[this]() noexcept { impl_->applyingToUi = false; }};

            impl_->sessionSelectors.value().clear();
            for (auto const& [sessionId, session] : impl_->stateHolder->stateCache().sessions)
            {
                impl_->sessionSelectors.value().push_back(
                    Settings::SectionSelectorOptions{
                        .thisSection = Settings::Section::Session,
                        .sessionId = sessionId,
                        .icon = !session.icon.empty() ? iconFromName(session.icon) : GeneratedSvgs::itsystem(),
                    }
                );
            }
            impl_->sessionSelectors.modify();

            impl_->generalSettings.loadFromState(impl_->stateHolder->stateCache());

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
                    impl_->currentSessionOptions.loadFromState(
                        impl_->stateHolder->stateCache().sessions.at(sessionId), true
                    );
                }
            }

            reloadInheritance();

            impl_->captureRestartBaseline();

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
    impl_->sshOptions.loadFromState(state.sshOptions.at(*key), false);
}
void Settings::loadSftpOptionsFromStateByKey(std::optional<std::string> const& key, Persistence::State const& state)
{
    if (!key || !state.sftpOptions.contains(*key))
        return;

    impl_->sftpOptions.loadFromState(state.sftpOptions.at(*key), false);
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

    impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKeys = *impl_->sshOptions.groupKeys;
    impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKeys = *impl_->sftpOptions.groupKeys;
    impl_->currentSessionOptions.termios.groupKeys = *impl_->termiosSettings.groupKeys;
    impl_->currentSessionOptions.terminalOptions.groupKeys = *impl_->terminalOptions.groupKeys;
    impl_->currentSessionOptions.queueOptions.groupKeys = *impl_->queueOptions.groupKeys;

    bool anyKeyChanged = false;
    if (impl_->sshOptions.groupKeys->empty())
    {
        Log::debug("SSH Options group keys empty, resetting session ssh options group key to nullopt.");
        impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKey = std::nullopt;
        anyKeyChanged = true;
    }
    if (impl_->sftpOptions.groupKeys->empty())
    {
        Log::debug("SFTP Options group keys empty, resetting session sftp options group key to nullopt.");
        impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKey = std::nullopt;
        anyKeyChanged = true;
    }
    if (impl_->termiosSettings.groupKeys->empty())
    {
        Log::debug("Termios Settings group keys empty, resetting session termios group key to nullopt.");
        impl_->currentSessionOptions.termios.groupKey = std::nullopt;
        anyKeyChanged = true;
    }
    if (impl_->terminalOptions.groupKeys->empty())
    {
        Log::debug("Terminal Options group keys empty, resetting session terminal options group key to nullopt.");
        impl_->currentSessionOptions.terminalOptions.groupKey = std::nullopt;
        anyKeyChanged = true;
    }
    if (impl_->queueOptions.groupKeys->empty())
    {
        Log::debug("Queue Options group keys empty, resetting session queue options group key to nullopt.");
        impl_->currentSessionOptions.queueOptions.groupKey = std::nullopt;
        anyKeyChanged = true;
    }

    loadTermiosSettingsFromStateByKey(impl_->termiosSettings.groupKey.value(), impl_->stateHolder->stateCache());
    loadSshSettingsFromStateByKey(impl_->sshOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadSftpOptionsFromStateByKey(impl_->sftpOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadTerminalOptionsFromStateByKey(impl_->terminalOptions.groupKey.value(), impl_->stateHolder->stateCache());
    loadQueueOptionsFromStateByKey(impl_->queueOptions.groupKey.value(), impl_->stateHolder->stateCache());

    if (anyKeyChanged)
        impl_->throttledReloadInheritance();
}

void Settings::reloadInheritance()
{
    auto assumeDefaultsFrom = [](auto& setting, auto const& stateMap, std::optional<std::string> const& groupKey)
    {
        if (groupKey)
        {
            auto iter = stateMap.find(*groupKey);
            if (iter != stateMap.end())
                return setting.assumeDefaultsFrom(iter->second);
        }
        setting.assumeDefaultsFrom({});
    };

    impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKeys = *impl_->sshOptions.groupKeys;
    impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKeys = *impl_->sftpOptions.groupKeys;
    impl_->currentSessionOptions.termios.groupKeys = *impl_->termiosSettings.groupKeys;
    impl_->currentSessionOptions.terminalOptions.groupKeys = *impl_->terminalOptions.groupKeys;
    impl_->currentSessionOptions.queueOptions.groupKeys = *impl_->queueOptions.groupKeys;

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

void Settings::save()
{
    impl_->stateHolder->save(
        [this](std::optional<std::string> const& error)
        {
            if (error)
            {
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("settings", "errorSavingSettingsHeader"),
                    .text =
                        fmt::format(fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error),
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "errorSavingSettings",
                });
            }

            impl_->saveInProgress = false;
            impl_->events->onSettingsChanged.modify();
            Nui::globalEventContext.executeActiveEventsImmediately();
        }
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
            save();
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

    Log::info("Settings::operator()()");
    Nui::ScopeExit onLeaveOperator(
        [this]() noexcept
        {
            Log::info("Settings::operator()() complete");
        }
    );

    try
    {
        // clang-format off
        return div{
            class_ = "settings-page-background-blocker",
            style = observe(impl_->events->settingsOpen)
                .generate(
                    [](bool isOpen) -> std::string
                    {
                        return isOpen ? "display: flex;" : "display: none;";
                    }
                ),
        }(
            div{
                class_ = "settings-loader-blocker",
                style = observe(impl_->initialLoadDone).generate(
                    [](bool initialLoadDone) -> std::string
                    {
                        return initialLoadDone ? "display: none;" : "display: flex;";
                    }
                ),
            }(
                Nui::Elements::span{}(language->get("settings", "loadingSettings"))
            ),
            impl_->newSessionDialog(),
            div{
                class_ = "settings-page",
            }(
                header(),
                div{
                    class_ = "settings-restart-banner",
                }(
                    observe(impl_->requiresRestart),
                    [](bool restartRequired) -> Nui::ElementRenderer
                    {
                        if (!restartRequired)
                            return Nui::nil();
                        return Snc::messageStrip({
                            .text = language->getObserved("settings", "restartRequiredMessage"),
                            .styleVariant = Snc::StyleVariant::Warning,
                        });
                    }
                ),
                div{
                    class_ = "settings-page-content",
                }(
                    side(),
                    div{
                        class_ = "settings-page-content main",
                    }(
                        observe(impl_->wasInitiallyLoaded),
                        [this](bool wasLoaded) {
                            if (!wasLoaded)
                                return div{}();
                            return sections();
                        }
                    )
                )
            )
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::operator(): {}", e.what());
        return div{}("Error loading settings page: "s + e.what());
    }
}

Nui::ElementRenderer Settings::sections()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;

    try
    {
        // clang-format off
        return div{
            class_ = "settings-page-sections"
        }(
            div{
                "data-settings-section"_attr = "GeneralSettings",
                style = observe(impl_->activeSection).generate(
                [this]() -> std::string
                {
                    return *impl_->activeSection == Section::GeneralSettings ? "" : "display: none;";
                }
            )}(impl_->generalSettings.render(
                [this](auto& currentGroupKey, auto& groupKeys) {addGroup(currentGroupKey, groupKeys);},
                [this](auto& currentGroupKey, auto& groupKeys) {removeGroup(currentGroupKey, groupKeys);},
                [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            )),
            div{
                "data-settings-section"_attr = "GlobalInheritables",
                style = observe(impl_->activeSection).generate(
                [this]() -> std::string
                {
                    return *impl_->activeSection == Section::GlobalInheritables ? "" : "display: none;";
                }
            )}(inheritableSettings()),
            div{
                "data-settings-section"_attr = "Session",
                style = observe(impl_->activeSection).generate(
                [this]() -> std::string
                {
                    return *impl_->activeSection == Section::Session ? "" : "display: none;";
                }
            )}(currentSession())
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::sections(): {}", e.what());
        return div{}("Error loading settings section: "s + e.what());
    }
}

Nui::ElementRenderer Settings::header()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    try
    {
        // clang-format off
        return div{
            class_ = "settings-page-header",
        }(
            iconPanel({
                .icon = GeneratedSvgs::actionsettings(),
                .color = "var(--theme-color)",
                .withBorder = true
            }),
            div{class_ = "title"}(language->getObserved("settings", "title")),
            div{
                class_ = "save-indicator",
                style = observe(impl_->saveInProgress).generate([](bool inProgress) {
                    return inProgress ? "visibility: visible;" : "visibility: hidden;";
                })
            }(
                span{}(language->getObserved("settings", "saving"))
            ),
            Snc::button({
                .icon = GeneratedSvgs::decline(),
                .attributes = {
                    onClick = [this]() {
                        const bool needsRestart = impl_->requiresRestart.value();
                        impl_->events->settingsOpen = false;
                        if (!needsRestart)
                            return;
                        impl_->confirmDialog->open({
                            .styleVariant = Snc::StyleVariant::Danger,
                            .headerText = language->get("settings", "settingsRestartRequiredHeader"),
                            .text = language->get("settings", "settingsRestartRequiredText"),
                            .buttons = ConfirmDialog::Button::Ok,
                            .neverShowAgainId = "settingsRestartRequiredWarning",
                        });
                    }
                },
                .styleVariant = Snc::StyleVariant::Transparent,
            })
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::header(): {}", e.what());
        return div{}("Error loading settings header: "s + e.what());
    }
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
    impl_->newSessionDialog.open({
        .onConfirm = [this](auto const& result)
        {
            Log::info("New session created: {}.", result.sessionName);
            const auto engineType = result.sessionType == NewSessionDialog::SessionType::ssh
                ? Persistence::TerminalEngineType::ssh
                : Persistence::TerminalEngineType::shell;
            impl_->stateHolder->stateCache().sessions[result.sessionName] =
                Persistence::SessionOptions::create(result.iconName, engineType);

            impl_->stateHolder->save(
                [this, result](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                            .neverShowAgainId = "errorSavingSettings",
                        });
                    }

                    impl_->sessionSelectors.value().push_back(
                        Settings::SectionSelectorOptions{
                            .thisSection = Settings::Section::Session,
                            .sessionId = result.sessionName,
                            .icon = iconFromName(result.iconName),
                        }
                    );
                    impl_->events->onSettingsChanged.modify();
                    impl_->sessionSelectors.modifyNow();
                }
            );
        },
    });
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
            impl_->currentSessionOptions.loadFromState(sessionIter->second, true);
            reloadInheritance();
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

    try
    {
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
                {
                    applySessionToState(*impl_->activeSession.value());
                    save();
                }
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
                        return iconPanel({
                            .icon = icon,
                            .color = active ? "var(--theme-color)" : "var(--background-color)",
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
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::sectionSelector(): {}", e.what());
        return div{}("Error loading section selector: "s + e.what());
    }
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
            GeneratedSvgs::settings(),
            span{}(language->getObserved("settings", "configuration"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::GeneralSettings,
            .icon = GeneratedSvgs::wrench(),
        }),
        sectionSelector({
            .thisSection = Settings::Section::GlobalInheritables,
            .icon = GeneratedSvgs::settings(),
        }),
        div{style = "width: calc(100% - 20px); border-top: 1px solid gray; margin-bottom: 20px; margin-top: 10px"}(),
        div{class_ = "configuration-text"}(
            GeneratedSvgs::itsystem(),
            span{}(language->getObserved("settings", "sessionsServers"))
        ),
        sectionSelector({
            .thisSection = Settings::Section::Add,
            .icon = GeneratedSvgs::add(),
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

Nui::ElementRenderer Settings::inheritableSettings()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    try
    {
        // clang-format off
        return fragment(
            Snc::messageStrip({
                .text = language->getObserved("settings", "inheritableSettingsInfoMessage"),
                .styleVariant = Snc::StyleVariant::Primary,
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sshOptions,
                .content = impl_->sshOptions.render(),
                .headerTitle = language->getObserved("settings", "sshOptionsGroupName"),
                .currentGroupKey = &impl_->sshOptions.groupKey,
                .groupKeys = &impl_->sshOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheritable,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sftpOptions,
                .content = impl_->sftpOptions.render(),
                .headerTitle = language->getObserved("settings", "sftpOptionsGroupName"),
                .currentGroupKey = &impl_->sftpOptions.groupKey,
                .groupKeys = &impl_->sftpOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheritable,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.terminalOptions,
                .content = impl_->terminalOptions.render(),
                .headerTitle = language->getObserved("settings", "terminalOptionsGroupName"),
                .currentGroupKey = &impl_->terminalOptions.groupKey,
                .groupKeys = &impl_->terminalOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheritable,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.queueOptions,
                .content = impl_->queueOptions.render(),
                .headerTitle = language->getObserved("settings", "queueOptionsGroupName"),
                .currentGroupKey = &impl_->queueOptions.groupKey,
                .groupKeys = &impl_->queueOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheritable,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.termios,
                .content = impl_->termiosSettings.render(),
                .headerTitle = language->getObserved("settings", "termiosGroupName"),
                .currentGroupKey = &impl_->termiosSettings.groupKey,
                .groupKeys = &impl_->termiosSettings.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheritable,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            })
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::inheritableSettings(): {}", e.what());
        return div{}("Error loading inheritable settings section: "s + e.what());
    }
}

void Settings::deleteActiveSession()
{
    if (!*impl_->activeSession)
        return;

    const auto sessionId = **impl_->activeSession;
    impl_->confirmDialog->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("settings", "deleteSessionConfirmHeader"),
            .text =
                fmt::format(fmt::runtime(language->get("settings", "deleteSessionConfirmText") + ": {}"), sessionId),
            .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
            .onClose = [this, sessionId](std::optional<ConfirmDialog::Button> optButton)
            {
                if (!optButton)
                    return;

                const auto button = *optButton;

                if (button != ConfirmDialog::Button::Ok)
                    return;

                impl_->stateHolder->stateCache().sessions.erase(sessionId);
                impl_->stateHolder->save(
                    [this](std::optional<std::string> const& error)
                    {
                        if (error)
                        {
                            impl_->confirmDialog->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = language->get("settings", "errorSavingSettingsHeader"),
                                .text = fmt::format(
                                    fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                                ),
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                        }
                    }
                );

                // delete session from session selectors:
                impl_->sessionSelectors.value().erase(
                    std::remove_if(
                        impl_->sessionSelectors.value().begin(),
                        impl_->sessionSelectors.value().end(),
                        [sessionId](auto const& item)
                        {
                            return item.sessionId == sessionId;
                        }
                    ),
                    impl_->sessionSelectors.value().end()
                );

                impl_->activeSession = std::nullopt;
                impl_->activeSection = Section::GeneralSettings;
                impl_->events->onSettingsChanged.modify();
                impl_->sessionSelectors.modifyNow();
            }}
    );
}

void Settings::renameActiveSession()
{
    if (!*impl_->activeSession)
        return;

    const auto oldSessionId = **impl_->activeSession;
    impl_->newSessionDialog.open({
        .onConfirm =
            [this, oldSessionId](NewSessionDialog::ConfirmResult const& result)
        {
            if (result.sessionName.empty() || result.sessionName == oldSessionId)
                return;

            const auto newSessionId = result.sessionName;

            if (impl_->stateHolder->stateCache().sessions.contains(newSessionId))
            {
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("settings", "sessionNameAlreadyExistsHeader"),
                    .text = language->get("settings", "sessionNameAlreadyExists"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            applySessionToState(oldSessionId);

            auto& sessions = impl_->stateHolder->stateCache().sessions;
            auto nodeHandle = sessions.extract(oldSessionId);
            nodeHandle.key() = newSessionId;
            sessions.insert(std::move(nodeHandle));

            impl_->stateHolder->save(
                [this](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }
                }
            );

            for (auto& selector : impl_->sessionSelectors.value())
            {
                if (selector.sessionId == oldSessionId)
                {
                    selector.sessionId = newSessionId;
                    break;
                }
            }

            impl_->activeSession = newSessionId;
            impl_->events->onSettingsChanged.modify();
            impl_->sessionSelectors.modifyNow();
        },
        .showIconPicker = false,
        .initialName = oldSessionId,
    });
}

void Settings::copyActiveSession()
{
    if (!*impl_->activeSession)
        return;

    const auto sourceSessionId = **impl_->activeSession;
    const auto sourceIcon = impl_->stateHolder->stateCache().sessions.count(sourceSessionId)
        ? impl_->stateHolder->stateCache().sessions.at(sourceSessionId).icon
        : std::string{"laptop"};
    impl_->newSessionDialog.open({
        .onConfirm =
            [this, sourceSessionId](NewSessionDialog::ConfirmResult const& result)
        {
            if (result.sessionName.empty() || result.sessionName == sourceSessionId)
                return;

            const auto newSessionId = result.sessionName;
            if (impl_->stateHolder->stateCache().sessions.contains(newSessionId))
            {
                impl_->confirmDialog->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("settings", "sessionNameAlreadyExistsHeader"),
                    .text = language->get("settings", "sessionNameAlreadyExists"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            applySessionToState(sourceSessionId);

            auto& sessions = impl_->stateHolder->stateCache().sessions;
            sessions[newSessionId] = sessions.at(sourceSessionId);
            sessions[newSessionId].icon = result.iconName;

            impl_->stateHolder->save(
                [this](std::optional<std::string> const& error)
                {
                    if (error)
                    {
                        impl_->confirmDialog->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("settings", "errorSavingSettingsHeader"),
                            .text = fmt::format(
                                fmt::runtime(language->get("settings", "errorSavingSettings") + ": {}"), *error
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                    }
                }
            );

            impl_->sessionSelectors.value().push_back(
                Settings::SectionSelectorOptions{
                    .thisSection = Settings::Section::Session,
                    .sessionId = newSessionId,
                    .icon = iconFromName(result.iconName),
                }
            );

            impl_->events->onSettingsChanged.modify();
            impl_->sessionSelectors.modifyNow();
        },
        .showIconPicker = true,
        .initialName = sourceSessionId,
        .initialIcon = sourceIcon,
    });
}

Nui::ElementRenderer Settings::currentSession()
{
    using namespace Nui;
    using namespace Nui::Elements;
    using namespace Nui::Attributes;
    using Nui::Elements::div;
    using Nui::Elements::span;

    try
    {
        // clang-format off
        auto overarchingSettings = fragment(
            div{
                class_ = "settings-session-deleter",
                style = "display: flex; gap: 8px; padding: 0 16px;"
            }(
                Snc::button({
                    .text = language->get("settings", "renameSessionButton"),
                    .icon = GeneratedSvgs::wrench(),
                    .attributes = {
                        onClick = [this]() {
                            renameActiveSession();
                        },
                    },
                }),
                Snc::button({
                    .text = language->get("settings", "copySessionButton"),
                    .icon = GeneratedSvgs::add(),
                    .attributes = {
                        onClick = [this]() {
                            copyActiveSession();
                        },
                    },
                }),
                Snc::button({
                    .text = language->get("settings", "deleteSessionButton"),
                    .icon = GeneratedSvgs::delete_(),
                    .attributes = {
                        onClick = [this]() {
                            deleteActiveSession();
                        },
                    },
                    .styleVariant = Snc::StyleVariant::Danger,
                })
            ),
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
            impl_->currentSessionOptions.layout(),
            div{
                class_ = "settings-visibility-box",
                style = observe(impl_->currentSessionOptions.terminalEngineType.state()).generate([](Persistence::TerminalEngineType type) {
                    return type == Persistence::TerminalEngineType::ssh ? "" : "display: none;";
                }),
            }(
                h1{
                    class_ = "settings-header"
                }(language->getObserved("settings", "sessionOptions", "sshSessionServerOptions")),
                subgroup({.onChange =
                        [this]()
                    {
                        onChange();
                    }},
                    fragment(
                        impl_->currentSessionOptions.sshSessionOptions.host(language->getObserved("settings", "sessionOptions", "host")),
                        impl_->currentSessionOptions.sshSessionOptions.port(language->getObserved("settings", "sessionOptions", "port")),
                        impl_->currentSessionOptions.sshSessionOptions.user(language->getObserved("settings", "sessionOptions", "user")),
                        impl_->currentSessionOptions.sshSessionOptions.sshKeyPrivate(language->getObserved("settings", "sessionOptions", "sshKeyPrivate")),
                        impl_->currentSessionOptions.sshSessionOptions.sshKeyPublic(language->getObserved("settings", "sessionOptions", "sshKeyPublic")),
                        impl_->currentSessionOptions.sshSessionOptions.openSftpByDefault(language->getObserved("settings", "sessionOptions", "openSftpByDefault")),
                        impl_->currentSessionOptions.sshSessionOptions.remoteFavorites(language->getObserved("settings", "sessionOptions", "remoteFavorites"))
                    )
                )
            ),
            div{
                class_ = "settings-visibility-box",
                style = observe(impl_->currentSessionOptions.terminalEngineType.state()).generate([](Persistence::TerminalEngineType type) {
                    return type == Persistence::TerminalEngineType::shell ? "" : "display: none;";
                }),
            }(
                h1{
                    class_ = "settings-header"
                }(language->getObserved("settings", "sessionOptions", "localSessionOptions")),
                subgroup({.onChange =
                        [this]()
                    {
                        onChange();
                    }},
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

        return fragment(
            Snc::messageStrip({
                .text = language->getObserved("settings", "inheritableSettingsInfoMessage"),
                .styleVariant = Snc::StyleVariant::Primary,
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.overarchingSettings,
                .content = std::move(overarchingSettings),
                .headerTitle = language->getObserved("settings", "sessionOptions", "basicSettings"),
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.sshOptions,
                .content = impl_->currentSessionOptions.sshSessionOptions.sshOptions.render(),
                .headerTitle = language->getObserved("settings", "sshOptionsGroupName"),
                .currentGroupKey = &impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKey,
                .groupKeys = &impl_->currentSessionOptions.sshSessionOptions.sshOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheriting,
                .engineTypeFilter = &impl_->currentSessionOptions.terminalEngineType.state(),
                .engineTypeFilterValue = Persistence::TerminalEngineType::ssh,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.sftpOptions,
                .content = impl_->currentSessionOptions.sshSessionOptions.sftpOptions.render(),
                .headerTitle = language->getObserved("settings", "sftpOptionsGroupName"),
                .currentGroupKey = &impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKey,
                .groupKeys = &impl_->currentSessionOptions.sshSessionOptions.sftpOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheriting,
                .engineTypeFilter = &impl_->currentSessionOptions.terminalEngineType.state(),
                .engineTypeFilterValue = Persistence::TerminalEngineType::ssh,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.terminalOptions,
                .content = impl_->currentSessionOptions.terminalOptions.render(),
                .headerTitle = language->getObserved("settings", "terminalOptionsGroupName"),
                .currentGroupKey = &impl_->currentSessionOptions.terminalOptions.groupKey,
                .groupKeys = &impl_->currentSessionOptions.terminalOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheriting,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.queueOptions,
                .content = impl_->currentSessionOptions.queueOptions.render(),
                .headerTitle = language->getObserved("settings", "queueOptionsGroupName"),
                .currentGroupKey = &impl_->currentSessionOptions.queueOptions.groupKey,
                .groupKeys = &impl_->currentSessionOptions.queueOptions.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheriting,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            }),
            group({
                .isCollapsed = impl_->collapsibleStates.sessionCollapsibles.termios,
                .content = impl_->currentSessionOptions.termios.render(),
                .headerTitle = language->getObserved("settings", "termiosGroupName"),
                .currentGroupKey = &impl_->currentSessionOptions.termios.groupKey,
                .groupKeys = &impl_->currentSessionOptions.termios.groupKeys,
                .inheritanceBehavior = SettingGroupParameters::InheritanceBehavior::Inheriting,
                .addGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    addGroup(currentGroupKey, groupKeys);
                },
                .removeGroup = [this](auto& currentGroupKey, auto& groupKeys){
                    removeGroup(currentGroupKey, groupKeys);
                },
                .onChangeGroup = [this](auto& currentGroupKey, auto const& newValue, auto& groupKeys, auto inheritanceBehavior){
                    onChangeGroup(currentGroupKey, newValue, groupKeys, inheritanceBehavior);
                }
            })
        );
        // clang-format on
    }
    catch (std::exception const& e)
    {
        Log::error("Exception in Settings::currentSession(): {}", e.what());
        return div{}("Error loading current session settings section: "s + e.what());
    }
}

void Settings::addGroup(
    Nui::Observed<std::optional<std::string>>& currentGroupKey,
    Nui::Observed<std::vector<std::string>>& groupKeys
)
{
    impl_->inputDialog->open({
        .whatFor = language->get("settings", "groupKey"),
        .prompt = language->get("settings", "enterGroupKeyPlaceholder"),
        .headerText = language->get("settings", "groupKey"),
        .isPassword = false,
        .onConfirm = [this, &currentGroupKey, &groupKeys](std::optional<std::string> const& result)
        {
            if (result && !result->empty())
            {
                currentGroupKey = *result;
                if (std::find(groupKeys->begin(), groupKeys->end(), *result) == groupKeys->end())
                {
                    groupKeys->push_back(*result);
                    groupKeys.modify();
                    reloadInheritance();
                    onChange();
                    Nui::globalEventContext.executeActiveEventsImmediately();
                }
                else
                {
                    // Key already exists, do nothing or show a message if needed
                    impl_->confirmDialog->open({
                        .styleVariant = Snc::StyleVariant::Warning,
                        .headerText = language->get("settings", "groupKeyExistsHeader"),
                        .text = language->get("settings", "groupKeyExistsText"),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                }
            }
        },
    });
}
void Settings::removeGroup(
    Nui::Observed<std::optional<std::string>>& currentGroupKey,
    Nui::Observed<std::vector<std::string>>& groupKeys
)
{
    const auto key = currentGroupKey->value_or(""s);
    if (key.empty())
        return;

    if (key == "default")
    {
        impl_->confirmDialog->open({
            .styleVariant = Snc::StyleVariant::Warning,
            .headerText = language->get("settings", "cannotDeleteDefaultGroupKeyHeader"),
            .text = language->get("settings", "cannotDeleteDefaultGroupKeyText"),
            .buttons = ConfirmDialog::Button::Ok,
            .neverShowAgainId = "cannotDeleteDefaultGroupKeyWarning",
        });
        return;
    }

    impl_->confirmDialog->open({
        .styleVariant = Snc::StyleVariant::Warning,
        .headerText = language->get("settings", "confirmDeleteGroupKeyHeader"),
        .text = language->get("settings", "confirmDeleteGroupKeyText"),
        .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
        .onClose = [this, &currentGroupKey, &groupKeys](std::optional<ConfirmDialog::Button> optButton)
        {
            if (!optButton)
                return;

            const auto btn = *optButton;

            if (!*currentGroupKey)
                return;

            const auto key = **currentGroupKey;

            if (btn == ConfirmDialog::Button::Ok)
            {
                groupKeys->erase(std::remove(groupKeys->begin(), groupKeys->end(), key), groupKeys->end());
                groupKeys.modify();
                if (!groupKeys->empty())
                {
                    currentGroupKey = groupKeys->front();
                }
                else
                {
                    currentGroupKey = "default"s;
                    groupKeys->push_back("default"s);
                    groupKeys.modify();
                }
                reloadInheritance();
                onChange();
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        },
    });
}

void Settings::onChangeGroup(
    Nui::Observed<std::optional<std::string>>& currentGroupKey,
    std::optional<std::string> const& newValue,
    Nui::Observed<std::vector<std::string>>&,
    SettingGroupParameters::InheritanceBehavior inheritanceBehavior
)
{
    const auto key = newValue.value_or(""s);
    Log::debug("Group key changed event: {}", key);
    if (key == "</>")
        currentGroupKey = std::nullopt;
    else
        currentGroupKey = key;
    if (inheritanceBehavior == SettingGroupParameters::InheritanceBehavior::Inheritable)
        reloadInheritables();
    else if (inheritanceBehavior == SettingGroupParameters::InheritanceBehavior::Inheriting)
    {
        if (*impl_->activeSession)
        {
            applySessionToState(**impl_->activeSession);
            save();
        }
        reloadInheritance();
    }
}