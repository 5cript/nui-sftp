#include <frontend/settings/ssh_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>

using namespace std::string_literals;

SshSessionOptions::SshSessionOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    InputDialog& inputDialog,
    MultiInputDialog& multiInputDialog
)
    : serverFactory_{
          factory.within({"sessionOptions", "basicSettings"}).within({"sessionOptions", "sshSessionServerOptions"})
      }
    , host{
          serverFactory_.identity({"sessionOptions", "host"}),
          onChange,
          valueReset(host, onChange, Persistence::SshSessionOptions{}.host)
      }
    , port{
          serverFactory_.identity({"sessionOptions", "port"}),
          onChange,
          nulloptReset(port, onChange),
          {
            .minValue = 1,
            .maxValue = 65535,
          }
      }
    , user{
          serverFactory_.identity({"sessionOptions", "user"}),
          onChange,
          nulloptReset(user, onChange)
      }
    , sshKeyPublic{
          serverFactory_.identity({"sessionOptions", "sshKeyPublic"}),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPublic, onChange)
      }
    , sshKeyPrivate{
          serverFactory_.identity({"sessionOptions", "sshKeyPrivate"}),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPrivate, onChange)
      }
    , openSftpByDefault{
          serverFactory_.identity({"sessionOptions", "openSftpByDefault"}),
          onChange,
          valueReset(openSftpByDefault, onChange, Persistence::SshSessionOptions{}.openSftpByDefault)
      }
    , remoteFavorites{
          serverFactory_.identity({"sessionOptions", "remoteFavorites"}),
          inputDialog,
          onChange,
          valueReset(remoteFavorites, onChange, Persistence::SshSessionOptions{}.remoteFavorites)
      }
    , maxReconnectAttempts{
          serverFactory_.identity({"sessionOptions", "maxReconnectAttempts"}),
          onChange,
          valueReset(
              maxReconnectAttempts, onChange, Persistence::SshSessionOptions{}.maxReconnectAttempts
          ),
          {
              .minValue = -1,
              .maxValue = 1000,
          }
      }
    , maxReconnectBackoffMs{
          serverFactory_.identity({"sessionOptions", "maxReconnectBackoffMs"}),
          onChange,
          valueReset(
              maxReconnectBackoffMs, onChange, Persistence::SshSessionOptions{}.maxReconnectBackoffMs
          ),
          {
              .minValue = 1000,
              .maxValue = 600000,
          }
      }
    , sshOptions{factory.within({"sshOptionsGroupName"}), onChange, inputDialog, multiInputDialog}
    , sftpOptions{factory.within({"sftpOptionsGroupName"}), onChange}
{}

void SshSessionOptions::applyToState(Persistence::SshSessionOptions& state) const
{
    assignIfValid(state.host, host);
    assignIfValid(state.port, port);
    assignIfValid(state.user, user);
    assignIfValid(state.sshKeyPublic, sshKeyPublic);
    assignIfValid(state.sshKeyPrivate, sshKeyPrivate);
    assignIfValid(state.openSftpByDefault, openSftpByDefault);
    state.remoteFavorites = remoteFavorites.value();
    assignIfValid(state.maxReconnectAttempts, maxReconnectAttempts);
    assignIfValid(state.maxReconnectBackoffMs, maxReconnectBackoffMs);
    sshOptions.applyToState(state.sshOptions.value());
    sftpOptions.applyToState(state.sftpOptions.value());

    state.sshOptions.ref(*sshOptions.groupKey);
    state.sftpOptions.ref(*sftpOptions.groupKey);
}

void SshSessionOptions::loadFromState(Persistence::SshSessionOptions const& state, bool loadRefs)
{
    host.value(state.host);
    port.value(state.port);
    user.value(state.user);
    sshKeyPublic.value(state.sshKeyPublic);
    sshKeyPrivate.value(state.sshKeyPrivate);
    openSftpByDefault.value(state.openSftpByDefault);
    remoteFavorites.value(state.remoteFavorites);
    maxReconnectAttempts.value(state.maxReconnectAttempts);
    maxReconnectBackoffMs.value(state.maxReconnectBackoffMs);
    sshOptions.loadFromState(state.sshOptions.value(), loadRefs);
    sftpOptions.loadFromState(state.sftpOptions.value(), loadRefs);

    if (loadRefs)
    {
        sshOptions.groupKey =
            state.sshOptions.hasReference() ? std::optional<std::string>{state.sshOptions.ref()} : std::nullopt;
        sftpOptions.groupKey =
            state.sftpOptions.hasReference() ? std::optional<std::string>{state.sftpOptions.ref()} : std::nullopt;
    }
}

void SshSessionOptions::assumeDefaultsFrom(Persistence::SshSessionOptions const& state)
{
    sshOptions.assumeDefaultsFrom(state.sshOptions.value());
    sftpOptions.assumeDefaultsFrom(state.sftpOptions.value());
}