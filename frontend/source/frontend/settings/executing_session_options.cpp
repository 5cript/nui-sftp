#include <frontend/settings/executing_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>

using namespace std::string_literals;

ExecutingSessionOptions::ExecutingSessionOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    InputDialog& inputDialog,
    MultiInputDialog& multiInputDialog
)
    : isPty{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "isPty"},
            {.labelKey = SettingKeyPath{"sessionOptions", "isPty"}}
        ),
        onChange,
        valueReset(isPty, onChange, Persistence::ExecutingSessionOptions{}.isPty)
    }
    , command{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "command"},
            {.labelKey = SettingKeyPath{"sessionOptions", "command"}}
        ),
        PathSettingType::File,
        onChange,
        valueReset(command, onChange, Persistence::ExecutingSessionOptions{}.command)
    }
    , arguments{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "arguments"},
            {.labelKey = SettingKeyPath{"sessionOptions", "arguments"}}
        ),
        inputDialog,
        onChange,
        nulloptReset(arguments, onChange)
    }
    , environment{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "environment"},
            {.labelKey = SettingKeyPath{"sessionOptions", "environmentVariables"}}
        ),
        multiInputDialog,
        onChange,
        nulloptReset(environment, onChange)
    }
    , exitTimeoutSeconds{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "exitTimeoutSeconds"},
            {.labelKey = SettingKeyPath{"sessionOptions", "exitTimeoutSeconds"}}
        ),
        onChange,
        valueReset(exitTimeoutSeconds, onChange, Persistence::ExecutingSessionOptions{}.exitTimeoutSeconds),
        {
            .minValue = 0,
            .stepValue = 1,
        }
    }
    , cleanEnvironment{
        factory.identity(
            {"sessionOptions", "executingSessionOptions", "cleanEnvironment"},
            {.labelKey = SettingKeyPath{"sessionOptions", "cleanEnvironment"}}
        ),
        onChange,
        valueReset(cleanEnvironment, onChange, Persistence::ExecutingSessionOptions{}.cleanEnvironment)
    }
{}

void ExecutingSessionOptions::applyToState(Persistence::ExecutingSessionOptions& state) const
{
    assignIfValid(state.isPty, isPty);
    assignIfValid(state.command, command);
    assignIfValid(state.arguments, arguments);
    assignIfValid(state.environment, environment);
    assignIfValid(state.exitTimeoutSeconds, exitTimeoutSeconds);
    assignIfValid(state.cleanEnvironment, cleanEnvironment);
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