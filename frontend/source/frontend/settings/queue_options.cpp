#include <frontend/settings/queue_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>

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