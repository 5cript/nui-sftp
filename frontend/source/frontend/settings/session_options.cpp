#include <frontend/settings/session_options.hpp>
#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/session_icon_options.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/icon_from_name.hpp>

#include <log/log.hpp>

using namespace std::string_literals;

namespace
{
    std::string engineTypeLabel(Persistence::TerminalEngineType type)
    {
        switch (type)
        {
            case Persistence::TerminalEngineType::shell:
                return language->get("settings", "sessionOptions", "terminalEngineTypeShell");
            case Persistence::TerminalEngineType::ssh:
                return "SSH";
            case Persistence::TerminalEngineType::ftp:
                return "FTP";
        }
        return "???";
    }

    /**
     * @brief The engine options of the session, replacing options of another engine type with empty ones.
     */
    template <typename EngineOptionsT>
    EngineOptionsT& engineOptionsOf(Persistence::SessionOptions& state)
    {
        if (auto* options = std::get_if<EngineOptionsT>(&state.engine))
            return *options;
        return state.engine.emplace<EngineOptionsT>();
    }

    /**
     * @brief The engine options of the session, or those a new session of the type starts with when the session is
     * of another type.
     */
    template <typename EngineOptionsT>
    EngineOptionsT engineOptionsOrNew(Persistence::SessionOptions const& state, Persistence::TerminalEngineType type)
    {
        if (auto const* options = std::get_if<EngineOptionsT>(&state.engine))
            return *options;
        return std::get<EngineOptionsT>(Persistence::SessionOptions::create(std::nullopt, type).engine);
    }

    /**
     * @brief The transfer options of the session, those a new SSH session starts with for a local session.
     */
    Persistence::Referenceable<Persistence::SftpOptions> transferOptionsOf(Persistence::SessionOptions const& state)
    {
        if (auto const* ftp = std::get_if<Persistence::FtpSessionOptions>(&state.engine))
            return ftp->sftpOptions;
        return engineOptionsOrNew<Persistence::SshSessionOptions>(state, Persistence::TerminalEngineType::ssh)
            .sftpOptions;
    }

    void applyTransferOptions(SftpOptions const& editor, Persistence::Referenceable<Persistence::SftpOptions>& state)
    {
        editor.applyToState(state.value());
        state.ref(*editor.groupKey);
    }
}

SessionOptions::SessionOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    ConfirmDialog& confirmDialog,
    InputDialog& newItemDialog,
    MultiInputDialog& multiInputDialog
)
    : basicSettingsFactory_{factory.within({"sessionOptions", "basicSettings"})}
    , terminalEngineType{
          {
              Persistence::TerminalEngineType::shell,
              Persistence::TerminalEngineType::ssh,
              Persistence::TerminalEngineType::ftp,
          },
          basicSettingsFactory_.identity({"sessionOptions", "terminalEngineType"}),
          onChange,
          valueReset(
              terminalEngineType,
              onChange,
              Persistence::SessionOptions{}.type
          ),
          engineTypeLabel
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
          basicSettingsFactory_.identity({"sessionOptions", "icon"}),
          onChange,
          valueReset(icon, onChange, Persistence::SessionOptions{}.icon),
            [](std::string const& icon)
            {
                return icon;
            },
            [](std::string const& icon) -> Nui::ElementRenderer
            {
                return iconFromName(icon);
            },
      }
    , orderBy{
          basicSettingsFactory_.identity({"sessionOptions", "orderBy"}),
          onChange,
          nulloptReset(orderBy, onChange)
      }
    , isStartupSession{
          basicSettingsFactory_.identity({"sessionOptions", "isStartupSession"}),
          onChange,
          valueReset(isStartupSession, onChange, Persistence::SessionOptions{}.startupSession)
      }
    , terminalOptions{factory.within({"terminalOptionsGroupName"}), onChange}
    , termios{factory.within({"termiosGroupName"}), onChange}
    , queueOptions{factory.within({"queueOptionsGroupName"}), onChange}
    , historyOptions{factory.within({"historyOptionsGroupName"}), onChange}
    , executingSessionOptions{
          basicSettingsFactory_.within({"sessionOptions", "localSessionOptions"})
              .forEngine(Persistence::TerminalEngineType::shell),
          onChange,
          newItemDialog,
          multiInputDialog
      }
    , sshSessionOptions{
          factory.forEngine(Persistence::TerminalEngineType::ssh),
          onChange,
          confirmDialog,
          newItemDialog,
          multiInputDialog
      }
    , ftpSessionOptions{
          factory.forEngine(Persistence::TerminalEngineType::ftp),
          onChange,
          confirmDialog,
          newItemDialog
      }
    , sftpOptions{
          factory.forEngines({Persistence::TerminalEngineType::ssh, Persistence::TerminalEngineType::ftp})
              .within({"sftpOptionsGroupName"}),
          onChange
      }
{}

void SessionOptions::applyToState(Persistence::SessionOptions& state) const
{
    assignIfValid(state.type, terminalEngineType);
    assignIfValid(state.icon, icon);
    assignIfValid(state.orderBy, orderBy);
    assignIfValid(state.startupSession, isStartupSession);
    terminalOptions.applyToState(state.terminalOptions.value());
    termios.applyToState(state.termios.value());
    queueOptions.applyToState(state.queueOptions.value());
    historyOptions.applyToState(state.historyOptions.value());

    switch (state.type)
    {
        case Persistence::TerminalEngineType::shell:
            executingSessionOptions.applyToState(engineOptionsOf<Persistence::ExecutingSessionOptions>(state));
            break;
        case Persistence::TerminalEngineType::ssh:
        {
            auto& ssh = engineOptionsOf<Persistence::SshSessionOptions>(state);
            sshSessionOptions.applyToState(ssh);
            applyTransferOptions(sftpOptions, ssh.sftpOptions);
            break;
        }
        case Persistence::TerminalEngineType::ftp:
        {
            auto& ftp = engineOptionsOf<Persistence::FtpSessionOptions>(state);
            ftpSessionOptions.applyToState(ftp);
            applyTransferOptions(sftpOptions, ftp.sftpOptions);
            break;
        }
    }

    Log::debug("SessionOptions::applyToState: setting references:");
    Log::debug(
        "TerminalOptions group key: {}", terminalOptions.groupKey->has_value() ? **terminalOptions.groupKey : "nullopt"
    );
    state.terminalOptions.ref(*terminalOptions.groupKey);
    Log::debug("Termios group key: {}", termios.groupKey->has_value() ? **termios.groupKey : "nullopt");
    state.termios.ref(*termios.groupKey);
    Log::debug("QueueOptions group key: {}", queueOptions.groupKey->has_value() ? **queueOptions.groupKey : "nullopt");
    state.queueOptions.ref(*queueOptions.groupKey);
    Log::debug(
        "HistoryOptions group key: {}", historyOptions.groupKey->has_value() ? **historyOptions.groupKey : "nullopt"
    );
    state.historyOptions.ref(*historyOptions.groupKey);
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
    historyOptions.loadFromState(state.historyOptions.value());

    if (std::holds_alternative<std::monostate>(state.engine))
        Log::warn("SessionOptions::loadFromState: engine variant holds no value.");

    executingSessionOptions.loadFromState(
        engineOptionsOrNew<Persistence::ExecutingSessionOptions>(state, Persistence::TerminalEngineType::shell),
        loadRefs
    );
    sshSessionOptions.loadFromState(
        engineOptionsOrNew<Persistence::SshSessionOptions>(state, Persistence::TerminalEngineType::ssh), loadRefs
    );
    ftpSessionOptions.loadFromState(
        engineOptionsOrNew<Persistence::FtpSessionOptions>(state, Persistence::TerminalEngineType::ftp), loadRefs
    );

    const auto transferOptions = transferOptionsOf(state);
    sftpOptions.loadFromState(transferOptions.value(), loadRefs);

    if (loadRefs)
    {
        terminalOptions.groupKey = state.terminalOptions.hasReference()
            ? std::optional<std::string>{state.terminalOptions.ref()}
            : std::nullopt;
        termios.groupKey =
            state.termios.hasReference() ? std::optional<std::string>{state.termios.ref()} : std::nullopt;
        queueOptions.groupKey =
            state.queueOptions.hasReference() ? std::optional<std::string>{state.queueOptions.ref()} : std::nullopt;
        historyOptions.groupKey =
            state.historyOptions.hasReference() ? std::optional<std::string>{state.historyOptions.ref()} : std::nullopt;
        sftpOptions.groupKey =
            transferOptions.hasReference() ? std::optional<std::string>{transferOptions.ref()} : std::nullopt;
    }
}

void SessionOptions::assumeDefaultsFrom(Persistence::SessionOptions const& state)
{
    terminalOptions.assumeDefaultsFrom(state.terminalOptions.value());
    termios.assumeDefaultsFrom(state.termios.value());
    queueOptions.assumeDefaultsFrom(state.queueOptions.value());
    historyOptions.assumeDefaultsFrom(state.historyOptions.value());
    if (auto const* executing = std::get_if<Persistence::ExecutingSessionOptions>(&state.engine))
        executingSessionOptions.assumeDefaultsFrom(*executing);
    else if (auto const* ssh = std::get_if<Persistence::SshSessionOptions>(&state.engine))
    {
        sshSessionOptions.assumeDefaultsFrom(*ssh);
        sftpOptions.assumeDefaultsFrom(ssh->sftpOptions.value());
    }
    else if (auto const* ftp = std::get_if<Persistence::FtpSessionOptions>(&state.engine))
    {
        ftpSessionOptions.assumeDefaultsFrom(*ftp);
        sftpOptions.assumeDefaultsFrom(ftp->sftpOptions.value());
    }
}