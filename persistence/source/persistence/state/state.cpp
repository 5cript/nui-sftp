#include <persistence/state/state.hpp>
#include <nlohmann/json.hpp>
#include <utility/visit_overloaded.hpp>

#include <tuple>

namespace Persistence
{
    State State::fullyResolve() const
    {
        State resolved{*this};

        auto fillDefaults = [](auto& target, auto const& source)
        {
            if (!target.hasReference())
                return;

            if (auto iter = source.find(target.ref()); iter != source.end())
                useDefaultsFrom(target, iter->second);
        };

        for (auto& [key, session] : resolved.sessions)
        {
            fillDefaults(session.terminalOptions, resolved.terminalOptions);
            fillDefaults(session.termios, resolved.termios);
            fillDefaults(session.queueOptions, resolved.queueOptions);

            Utility::visitOverloaded(
                session.engine,
                [&](std::monostate)
                {
                    // Nothing to do here
                },
                [&](ExecutingSessionOptions&)
                {
                    // Nothing to do here
                },
                [&](SshSessionOptions& session)
                {
                    fillDefaults(session.sshOptions, resolved.sshOptions);
                    fillDefaults(session.sftpOptions, resolved.sftpOptions);
                }
            );
        }

        return resolved;
    }
}