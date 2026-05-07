#include <frontend/onboarding/onboarding.hpp>

#include <utility/language.hpp>
#include <log/log.hpp>

#include <script-nui-components/spotlight_overlay.hpp>

#include <nui/event_system/listen.hpp>

namespace Frontend
{
    struct Onboarding::Implementation
    {
        Persistence::StateHolder* stateHolder;
        FrontendEvents* events;
        ScriptNuiComponents::SpotlightOverlay overlay;
        Nui::ListenRemover<decltype(FrontendEvents::settingsOpen)> settingsOpenListener{};
        Nui::ListenRemover<decltype(FrontendEvents::onNewSession)> newSessionListener{};
        Nui::ListenRemover<decltype(FrontendEvents::settingsInitialLoadComplete)> settingsLoadedListener{};
        OnboardingStep step{OnboardingStep::Inactive};
        bool persisted{false};

        Implementation(Persistence::StateHolder* stateHolder, FrontendEvents* events)
            : stateHolder{stateHolder}
            , events{events}
        {}

        bool shouldAutoStart() const
        {
            return !stateHolder->stateCache().uiOptions.onboardingCompleted;
        }

        ScriptNuiComponents::SpotlightOptions buildStep1Options()
        {
            return ScriptNuiComponents::SpotlightOptions{
                .targetElementId = OnboardingTargets::toolbarSettingsButtonId,
                .title = language->get("onboarding", "step1Title"),
                .bodyText = language->get("onboarding", "step1Body"),
                .stepCounter = ScriptNuiComponents::SpotlightStepCounter{1, 2},
                .ctaLabel = language->get("onboarding", "next"),
                .skipLabel = language->get("onboarding", "skip"),
                // Backdrop click is disabled: accidental clicks (or clicks
                // forwarded through the cutout to the highlighted button)
                // would otherwise terminate the flow rather than advance it.
                // Dismiss paths remain via Skip, X, and Esc.
                .dismissOnBackdropClick = false,
                .onAdvance =
                    [this]()
                {
                    advanceFromStep1();
                },
                .onDismiss =
                    [this]()
                {
                    dismiss();
                },
            };
        }

        ScriptNuiComponents::SpotlightOptions buildStep2Options()
        {
            return ScriptNuiComponents::SpotlightOptions{
                .targetElementId = OnboardingTargets::settingsAddNewButtonId,
                .title = language->get("onboarding", "step2Title"),
                .bodyText = language->get("onboarding", "step2Body"),
                .stepCounter = ScriptNuiComponents::SpotlightStepCounter{2, 2},
                .ctaLabel = language->get("onboarding", "finish"),
                .skipLabel = language->get("onboarding", "skip"),
                .dismissOnBackdropClick = false,
                .onAdvance =
                    [this]()
                {
                    finish();
                },
                .onDismiss =
                    [this]()
                {
                    dismiss();
                },
            };
        }

        void enterStep1()
        {
            step = OnboardingStep::OpenSettings;
            // The user clicking the actual highlighted button toggles
            // settingsOpen — the listener picks that up and advances.
            settingsOpenListener = Nui::smartListen(
                events->settingsOpen,
                [this](bool open)
                {
                    if (open && step == OnboardingStep::OpenSettings)
                        beginAwaitingSettingsLoad();
                }
            );
            overlay.show(buildStep1Options());
        }

        void beginAwaitingSettingsLoad()
        {
            step = OnboardingStep::WaitingForSettingsLoad;
            // Hide the spotlight while the settings panel runs its 3-pass
            // reveal (loader -> heavy subtree -> loader hidden). Showing
            // step 2 over the loader is visually confusing because the
            // target button is not yet visible.
            overlay.hide();

            // If a previous open already latched the load-complete signal
            // (subsequent opens are instant), advance immediately.
            if (events->settingsInitialLoadComplete.value())
            {
                enterStep2();
                return;
            }

            settingsLoadedListener = Nui::smartListen(
                events->settingsInitialLoadComplete,
                [this](bool loaded)
                {
                    if (loaded && step == OnboardingStep::WaitingForSettingsLoad)
                        enterStep2();
                }
            );
        }

        void enterStep2()
        {
            step = OnboardingStep::AddNewServer;
            settingsLoadedListener = {};
            // Clicking the highlighted "Add New" button writes a session
            // name into onNewSession; advance when it changes.
            newSessionListener = Nui::smartListen(
                events->onNewSession,
                [this](std::string const& sessionName)
                {
                    if (!sessionName.empty() && step == OnboardingStep::AddNewServer)
                        finish();
                }
            );
            overlay.show(buildStep2Options());
        }

        void advanceFromStep1()
        {
            // CTA path: open settings on the user's behalf if they used
            // "Next" instead of clicking the highlighted button.
            if (!events->settingsOpen.value())
                events->settingsOpen = true;
            // The settings listener will pick that up and call
            // beginAwaitingSettingsLoad; no need to invoke it directly.
        }

        void finish()
        {
            persistFlag();
            shutdown(OnboardingStep::Done);
        }

        void dismiss()
        {
            persistFlag();
            shutdown(OnboardingStep::Done);
        }

        void shutdown(OnboardingStep terminal)
        {
            step = terminal;
            settingsOpenListener = {};
            newSessionListener = {};
            settingsLoadedListener = {};
            overlay.hide();
        }

        void persistFlag()
        {
            if (persisted)
                return;
            persisted = true;
            stateHolder->loadModifySave(
                [](Persistence::State& state)
                {
                    state.uiOptions.onboardingCompleted = true;
                },
                [](std::optional<std::string> const& error)
                {
                    if (error)
                        Log::warn("Failed to persist onboarding completion flag: {}", *error);
                }
            );
        }
    };

    Onboarding::Onboarding(Persistence::StateHolder* stateHolder, FrontendEvents* events)
        : impl_{std::make_unique<Implementation>(stateHolder, events)}
    {}

    void Onboarding::maybeStart()
    {
        if (impl_->step != OnboardingStep::Inactive)
            return;
        if (!impl_->shouldAutoStart())
            return;
        impl_->enterStep1();
    }

    void Onboarding::start()
    {
        if (impl_->step != OnboardingStep::Inactive)
            return;
        impl_->enterStep1();
    }

    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL(Onboarding);
} // namespace Frontend
