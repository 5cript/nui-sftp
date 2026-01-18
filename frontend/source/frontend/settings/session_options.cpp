#include <frontend/settings/session_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/session_icon_options.hpp>

#include <utility/enum_string_convert.hpp>

using namespace std::string_literals;

SessionOptions::SessionOptions(std::function<void()> const& onChange)
    : terminalEngineType{
          {
              Persistence::TerminalEngineType::shell,
              Persistence::TerminalEngineType::ssh,
          },
          language->getObserved("settings", "sessionOptions", "terminalEngineTypeHelpText"),
          onChange,
          valueReset(
              terminalEngineType,
              onChange,
              Persistence::SessionOptions{}.type
          ),
          [](Persistence::TerminalEngineType const& v)
          {
              return Utility::enumToString(v);
        }
      }
    , icon{
        [](){
            std::vector<std::string> icons;
            for (const auto iconName : sessionIconOptions)
            {
                icons.push_back(std::string{iconName});
            }
            return icons;
        }(),
          language->getObserved("settings", "sessionOptions", "iconHelpText"),
          onChange,
          valueReset(icon, onChange, Persistence::SessionOptions{}.icon),
            [](std::string const& icon)
            {
                return icon;
            },
            [](std::string const& v)
            {
                return std::optional<std::string>{v};
            },
      }
    , orderBy{
          language->getObserved("settings", "sessionOptions", "orderByHelpText"),
          onChange,
          nulloptReset(orderBy, onChange)
      }
    , isStartupSession{
          language->getObserved("settings", "sessionOptions", "isStartupSessionHelpText"),
          onChange,
          valueReset(isStartupSession, onChange, Persistence::SessionOptions{}.startupSession)
      }
    , terminalOptions{onChange}
    , termios{onChange}
    , queueOptions{onChange}
    , executingSessionOptions{onChange}
    , sshSessionOptions{onChange}
{}

void SessionOptions::applyToState(Persistence::SessionOptions& state) const
{
    state.type = terminalEngineType.value();
    state.icon = icon.value();
    state.orderBy = orderBy.value();
    state.startupSession = isStartupSession.value();
    terminalOptions.applyToState(state.terminalOptions.value());
    termios.applyToState(state.termios.value());
    queueOptions.applyToState(state.queueOptions.value());

    if (state.type == Persistence::TerminalEngineType::ssh)
    {
        sshSessionOptions.applyToState(state.engine.emplace<Persistence::SshSessionOptions>());
    }
    else if (state.type == Persistence::TerminalEngineType::shell)
    {
        executingSessionOptions.applyToState(state.engine.emplace<Persistence::ExecutingSessionOptions>());
    }

    state.terminalOptions.ref(*terminalOptions.groupKey);
    state.termios.ref(*termios.groupKey);
    state.queueOptions.ref(*queueOptions.groupKey);
}

void SessionOptions::loadFromState(Persistence::SessionOptions const& state, bool loadRefs)
{
    terminalEngineType.value(state.type);
    icon.value(state.icon);
    orderBy.value(state.orderBy);
    isStartupSession.value(state.startupSession);
    terminalOptions.loadFromState(state.terminalOptions.value());
    termios.loadFromState(state.termios.value());
    queueOptions.loadFromState(state.queueOptions.value());

    if (state.type == Persistence::TerminalEngineType::ssh)
    {
        sshSessionOptions.loadFromState(std::get<Persistence::SshSessionOptions>(state.engine), loadRefs);
    }
    else if (state.type == Persistence::TerminalEngineType::shell)
    {
        executingSessionOptions.loadFromState(std::get<Persistence::ExecutingSessionOptions>(state.engine), loadRefs);
    }

    if (loadRefs)
    {
        terminalOptions.groupKey = state.terminalOptions.hasReference()
            ? std::optional<std::string>{state.terminalOptions.ref()}
            : std::nullopt;
        termios.groupKey =
            state.termios.hasReference() ? std::optional<std::string>{state.termios.ref()} : std::nullopt;
        queueOptions.groupKey =
            state.queueOptions.hasReference() ? std::optional<std::string>{state.queueOptions.ref()} : std::nullopt;
    }
}

void SessionOptions::assumeDefaultsFrom(Persistence::SessionOptions const& state)
{
    terminalOptions.assumeDefaultsFrom(state.terminalOptions.value());
    termios.assumeDefaultsFrom(state.termios.value());
    queueOptions.assumeDefaultsFrom(state.queueOptions.value());
    if (state.type == Persistence::TerminalEngineType::shell)
    {
        executingSessionOptions.assumeDefaultsFrom(std::get<Persistence::ExecutingSessionOptions>(state.engine));
    }
    else if (state.type == Persistence::TerminalEngineType::ssh)
    {
        sshSessionOptions.assumeDefaultsFrom(std::get<Persistence::SshSessionOptions>(state.engine));
    }
}