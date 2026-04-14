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
    , liveQueuePageSize{
          language->getObserved("settings", "queueOptions", "liveQueuePageSizeHelpText"),
          onChange,
          valueReset(liveQueuePageSize, onChange, 200),
          NumberSetting<int, true>::ConstructionArgs{
              .minValue = 1,
              .maxValue = 1000,
          },
      }
{}

void QueueOptions::applyToState(Persistence::QueueOptions& state) const
{
    assignIfValid(state.autoRemoveCompletedOperations, autoRemoveCompletedOperations);
    assignIfValid(state.startInPausedState, startInPausedState);
    assignIfValid(state.liveQueuePageSize, liveQueuePageSize);
}

void QueueOptions::loadFromState(Persistence::QueueOptions const& state)
{
    autoRemoveCompletedOperations.value(state.autoRemoveCompletedOperations);
    startInPausedState.value(state.startInPausedState);
    liveQueuePageSize.value(state.liveQueuePageSize);
}

void QueueOptions::assumeDefaultsFrom(Persistence::QueueOptions const& state)
{
    autoRemoveCompletedOperations.inherit(state.autoRemoveCompletedOperations);
    startInPausedState.inherit(state.startInPausedState);
    liveQueuePageSize.inherit(state.liveQueuePageSize);
}

Nui::ElementRenderer QueueOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        autoRemoveCompletedOperations(
            language->getObserved("settings", "queueOptions", "autoRemoveCompletedOperations")
        ),
        startInPausedState(language->getObserved("settings", "queueOptions", "startInPausedState")),
        liveQueuePageSize(language->getObserved("settings", "queueOptions", "liveQueuePageSize"))
    );
}