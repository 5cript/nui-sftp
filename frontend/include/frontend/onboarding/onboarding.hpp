#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/events/frontend_events.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>

namespace Frontend
{
    /** @brief Strongly-typed step identifier for the onboarding state
     *         machine. Adding a new step is as simple as appending an
     *         enumerator and wiring it in `Onboarding::Implementation`. */
    enum class OnboardingStep : unsigned
    {
        Inactive = 0,
        OpenSettings = 1,
        AddNewServer = 2,
        Done = 3,
    };

    /** @brief DOM ids the onboarding orchestrator targets. The application
     *         attaches these ids to the actual buttons; keeping the
     *         constants here makes the binding explicit and grep-friendly. */
    namespace OnboardingTargets
    {
        inline constexpr char toolbarSettingsButtonId[] = "toolbar-settings-button";
        inline constexpr char settingsAddNewButtonId[] = "settings-add-new-button";
    }

    /** @brief First-launch onboarding flow. Auto-fires once when no servers
     *         are configured and the user has not previously completed or
     *         dismissed the flow. Persists the completion flag so it never
     *         reappears. */
    class Onboarding
    {
      public:
        Onboarding(Persistence::StateHolder* stateHolder, FrontendEvents* events);
        ROAR_PIMPL_SPECIAL_FUNCTIONS(Onboarding);

        /** @brief Evaluate trigger conditions and start the flow if they
         *         hold. Idempotent. Call from `MainPage::onSetupComplete`
         *         after persistence has loaded. */
        void maybeStart();

        /** @brief Force-start the flow (debug / manual replay entry point). */
        void start();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
} // namespace Frontend
