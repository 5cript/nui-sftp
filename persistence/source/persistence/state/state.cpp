#include <persistence/state/state.hpp>
#include <nlohmann/json.hpp>
#include <utility/visit_overloaded.hpp>

#include <tuple>

namespace Persistence
{
    void to_json(nlohmann::json& j, State const& state)
    {
        j = nlohmann::json::object();

        j["terminalOptions"] = state.terminalOptions;
        j["sessions"] = state.sessions;
        j["termios"] = state.termios;
        j["sshOptions"] = state.sshOptions;
        j["sftpOptions"] = state.sftpOptions;
        j["sshSessionOptions"] = state.sshSessionOptions;
        j["uiOptions"] = state.uiOptions;
        j["localFilesystemOptions"] = state.localFilesystemOptions;
        j["logLevel"] = [&]() {
            return Log::levelToString(state.logLevel);
        }();
        j["queueOptions"] = state.queueOptions;
    }
    void from_json(nlohmann::json const& j, State& state)
    {
        if (j.contains("terminalOptions"))
            j.at("terminalOptions").get_to(state.terminalOptions);

        if (j.contains("sessions"))
            j.at("sessions").get_to(state.sessions);

        if (j.contains("termios"))
            j.at("termios").get_to(state.termios);

        if (j.contains("sshOptions"))
            j.at("sshOptions").get_to(state.sshOptions);

        if (j.contains("sftpOptions"))
            j.at("sftpOptions").get_to(state.sftpOptions);

        if (j.contains("sshSessionOptions"))
            j.at("sshSessionOptions").get_to(state.sshSessionOptions);

        if (j.contains("uiOptions"))
            j.at("uiOptions").get_to(state.uiOptions);

        if (j.contains("logLevel"))
            state.logLevel = Log::levelFromString(j.at("logLevel").get<std::string>());

        if (j.contains("queueOptions"))
            j.at("queueOptions").get_to(state.queueOptions);

        if (j.contains("localFilesystemOptions"))
            j.at("localFilesystemOptions").get_to(state.localFilesystemOptions);
    }

    State State::fullyResolve() const
    {
        State resolved{*this};

        auto fillDefaults = [](auto& target, auto const& source) {
            if (!target.hasReference())
                return;

            if (auto iter = source.find(target.ref()); iter != source.end())
                target.useDefaultsFrom(iter->second);
        };

        for (auto& [key, session] : resolved.sessions)
        {
            fillDefaults(session.terminalOptions, resolved.terminalOptions);
            fillDefaults(session.termios, resolved.termios);
            fillDefaults(session.queueOptions, resolved.queueOptions);

            Utility::visitOverloaded(
                session.engine,
                [&](std::monostate) {
                    // Nothing to do here
                },
                [&](ExecutingTerminalEngine&) {
                    // Nothing to do here
                },
                [&](SshTerminalEngine& engine) {
                    fillDefaults(engine.sshSessionOptions, resolved.sshSessionOptions);
                    fillDefaults(engine.sshSessionOptions->sshOptions, resolved.sshOptions);
                    fillDefaults(engine.sshSessionOptions->sftpOptions, resolved.sftpOptions);
                });
        }

        return resolved;
    }
}