#include <frontend/settings/queue_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

QueueOptions::QueueOptions(std::function<void()> const& onChange)
    : autoRemoveCompletedOperations{
          language->getObserved("settings", "queueOptions", "autoRemoveCompletedOperationsHelpText"),
          onChange,
          valueReset(autoRemoveCompletedOperations, onChange, false),
      }
    , startInPausedState{
          language->getObserved("settings", "queueOptions", "startInPausedStateHelpText"),
          onChange,
          valueReset(startInPausedState, onChange, true),
      }
{}

void QueueOptions::applyToState(Persistence::QueueOptions& state) const
{
    assignIfValid(state.autoRemoveCompletedOperations, autoRemoveCompletedOperations);
    assignIfValid(state.startInPausedState, startInPausedState);
}

void QueueOptions::loadFromState(Persistence::QueueOptions const& state)
{
    autoRemoveCompletedOperations.value(state.autoRemoveCompletedOperations);
    startInPausedState.value(state.startInPausedState);
}

void QueueOptions::assumeDefaultsFrom(Persistence::QueueOptions const& state)
{
    autoRemoveCompletedOperations.inherit(state.autoRemoveCompletedOperations);
    startInPausedState.inherit(state.startInPausedState);
}

Nui::ElementRenderer QueueOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        autoRemoveCompletedOperations(
            language->getObserved("settings", "queueOptions", "autoRemoveCompletedOperations")
        ),
        startInPausedState(language->getObserved("settings", "queueOptions", "startInPausedState"))
    );
}