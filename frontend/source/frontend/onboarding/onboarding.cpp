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
                        enterStep2();
                }
            );
            overlay.show(buildStep1Options());
        }

        void enterStep2()
        {
            step = OnboardingStep::AddNewServer;
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
            // The settings listener will pick that up and call enterStep2;
            // no need to call it directly here.
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
