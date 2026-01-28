#include <frontend/settings/executing_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>

using namespace std::string_literals;

ExecutingSessionOptions::ExecutingSessionOptions(std::function<void()> const& onChange)
    : isPty{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "isPtyHelpText"),
        onChange,
        valueReset(isPty, onChange, Persistence::ExecutingSessionOptions{}.isPty)
    }
    , command{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "commandHelpText"),
        PathSettingType::File,
        onChange,
        valueReset(command, onChange, Persistence::ExecutingSessionOptions{}.command)
    }
    , arguments{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "argumentsHelpText"),
        onChange,
        nulloptReset(arguments, onChange)
    }
    , environment{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "environmentHelpText"),
        onChange,
        nulloptReset(environment, onChange)
    }
    , exitTimeoutSeconds{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "exitTimeoutSecondsHelpText"),
        onChange,
        valueReset(exitTimeoutSeconds, onChange, Persistence::ExecutingSessionOptions{}.exitTimeoutSeconds),
        {
            .minValue = 0,
            .stepValue = 1,
        }
    }
    , cleanEnvironment{
        language->getObserved("settings", "sessionOptions", "executingSessionOptions", "cleanEnvironmentHelpText"),
        onChange,
        valueReset(cleanEnvironment, onChange, Persistence::ExecutingSessionOptions{}.cleanEnvironment)
    }
{}

void ExecutingSessionOptions::applyToState(Persistence::ExecutingSessionOptions& state) const
{
    state.isPty = isPty.value();
    state.command = command.value();
    state.arguments = arguments.value();
    state.environment = environment.value();
    state.exitTimeoutSeconds = exitTimeoutSeconds.value();
    state.cleanEnvironment = cleanEnvironment.value();
}

void ExecutingSessionOptions::loadFromState(Persistence::ExecutingSessionOptions const& state, bool)
{
    isPty.value(state.isPty);
    command.value(state.command);
    arguments.value(state.arguments);
    environment.value(state.environment);
    exitTimeoutSeconds.value(state.exitTimeoutSeconds);
    cleanEnvironment.value(state.cleanEnvironment);
}

void ExecutingSessionOptions::assumeDefaultsFrom(Persistence::ExecutingSessionOptions const& state)
{
    arguments.inherit(state.arguments);
    environment.inherit(state.environment);
}