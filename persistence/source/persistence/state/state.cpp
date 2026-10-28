#include <persistence/state/state.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <string_view>

namespace Persistence
{
    namespace
    {
        bool hasKnownEngineType(nlohmann::json const& session)
        {
            if (!session.is_object())
                return false;
            const auto type = session.find("type");
            return type != session.end() && type->is_string() &&
                terminalEngineTypeFromName(type->get_ref<std::string const&>()).has_value();
        }

        std::map<std::string, nlohmann::json> setAsideUnreadSessions(nlohmann::json& config)
        {
            std::map<std::string, nlohmann::json> unread{};
            if (!config.is_object())
                return unread;
            const auto sessions = config.find("sessions");
            if (sessions == config.end() || !sessions->is_object())
                return unread;

            for (auto session = sessions->begin(); session != sessions->end();)
            {
                if (hasKnownEngineType(*session))
                {
                    ++session;
                    continue;
                }
                unread.emplace(session.key(), std::move(session.value()));
                session = sessions->erase(session);
            }
            return unread;
        }
    }

    void from_json(nlohmann::json const& j, State& state)
    {
        auto readable = j;
        state.unreadSessions = setAsideUnreadSessions(readable);
        SharedData::from_json(readable, state);
    }

    void to_json(nlohmann::json& j, State const& state)
    {
        SharedData::to_json(j, state);
        auto& sessions = j["sessions"];
        for (auto const& [name, session] : state.unreadSessions)
            sessions.emplace(name, session);
    }

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

    bool State::migrateTerminalRendererFromDom()
    {
        constexpr std::string_view migration = "terminalRendererFromDom";
        if (!appliedMigrations)
            appliedMigrations = std::vector<std::string>{};
        if (std::ranges::find(*appliedMigrations, migration) != appliedMigrations->end())
            return false;
        appliedMigrations->emplace_back(migration);

#ifdef __linux__
        const auto moveToWebgl = [](TerminalOptions& options)
        {
            if (options.renderer == "dom")
                options.renderer = "webgl";
        };
        for (auto& [name, options] : terminalOptions)
            moveToWebgl(options);
        for (auto& [name, session] : sessions)
            moveToWebgl(session.terminalOptions.value());
#endif
        return true;
    }

    bool State::gatherSessionLayouts()
    {
        if (sessionLayouts)
            return false;
        sessionLayouts = SessionLayouts{};

        for (auto const& [serverName, session] : sessions)
        {
            if (!session.layouts)
                continue;
            auto& layouts = sessionLayouts->of(session.type);
            for (auto const& [layoutName, layout] : *session.layouts)
            {
                auto name = layoutName;
                for (int attempt = 1; layouts.contains(name) && layouts[name] != layout; ++attempt)
                {
                    name = attempt == 1 ? fmt::format("{} ({})", layoutName, serverName)
                                        : fmt::format("{} ({}) {}", layoutName, serverName, attempt);
                }
                layouts.emplace(name, layout);
            }
        }
        return true;
    }
}
