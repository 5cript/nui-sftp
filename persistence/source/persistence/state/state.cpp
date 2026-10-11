#include <persistence/state/state.hpp>
#include <nlohmann/json.hpp>

namespace Persistence
{
    void InheritableReferences::applyTo(SessionOptions& session) const
    {
        session.terminalOptions.ref(Reference{terminalOptions});
        session.termios.ref(Reference{termios});
        session.queueOptions.ref(Reference{queueOptions});
        session.historyOptions.ref(Reference{historyOptions});
        if (auto* ssh = std::get_if<SshSessionOptions>(&session.engine))
        {
            ssh->sshOptions.ref(Reference{sshOptions});
            ssh->sftpOptions.ref(Reference{sftpOptions});
        }
    }

    State State::fullyResolve() const
    {
        State resolved{*this};
        for (auto& [key, session] : resolved.sessions)
            resolved.resolveReferencesOf(session);
        return resolved;
    }

    void State::resolveReferencesOf(SessionOptions& session) const
    {
        auto fillDefaults = [](auto& target, auto const& source)
        {
            if (!target.hasReference())
                return;

            if (auto iter = source.find(target.ref()); iter != source.end())
                useDefaultsFrom(target, iter->second);
        };

        fillDefaults(session.terminalOptions, terminalOptions);
        fillDefaults(session.termios, termios);
        fillDefaults(session.queueOptions, queueOptions);
        fillDefaults(session.historyOptions, historyOptions);

        if (auto* ssh = std::get_if<SshSessionOptions>(&session.engine))
        {
            fillDefaults(ssh->sshOptions, sshOptions);
            fillDefaults(ssh->sftpOptions, sftpOptions);
        }
    }

    void State::writeLegacyUsers()
    {
        for (auto& [name, session] : sessions)
        {
            if (auto* ssh = std::get_if<SshSessionOptions>(&session.engine))
                ssh->writeLegacyUser();
        }
        if (lastDirectConnect)
            lastDirectConnect->writeLegacyUser();
    }
}
