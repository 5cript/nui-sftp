#include <frontend/settings/queue_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

QueueOptions::QueueOptions(SettingFactory const& factory, std::function<void()> const& onChange)
    : factory_{factory.forGroup(groupKey)}
    , autoRemoveCompletedOperations{
          factory_.identity({"queueOptions", "autoRemoveCompletedOperations"}),
          onChange,
          valueReset(autoRemoveCompletedOperations, onChange, false),
      }
    , startInPausedState{
          factory_.identity({"queueOptions", "startInPausedState"}),
          onChange,
          valueReset(startInPausedState, onChange, true),
      }
    , liveQueuePageSize{
          factory_.identity({"queueOptions", "liveQueuePageSize"}),
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
        autoRemoveCompletedOperations(),
        startInPausedState(),
        liveQueuePageSize()
    );
}