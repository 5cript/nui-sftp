#include <frontend/settings/ssh_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>

using namespace std::string_literals;

SshSessionOptions::SshSessionOptions(std::function<void()> const& onChange, InputDialog& inputDialog, MultiInputDialog& multiInputDialog)
    : host{
          language->getObserved("settings", "sessionSettings", "host"),
          onChange,
          valueReset(host, onChange, Persistence::SshSessionOptions{}.host)
      }
    , port{
          language->getObserved("settings", "sessionSettings", "port"),
          onChange,
          nulloptReset(port, onChange),
          {
            .minValue = 1,
            .maxValue = 65535,
          }
      }
    , user{
          language->getObserved("settings", "sessionSettings", "user"),
          onChange,
          nulloptReset(user, onChange)
      }
    , sshKeyPublic{
          language->getObserved("settings", "sessionSettings", "sshKeyPublic"),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPublic, onChange)
      }
    , sshKeyPrivate{
          language->getObserved("settings", "sessionSettings", "sshKeyPrivate"),
          PathSettingType::File,
          onChange,
          nulloptReset(sshKeyPrivate, onChange)
      }
    , openSftpByDefault{
          language->getObserved("settings", "sessionSettings", "openSftpByDefault"),
          onChange,
          valueReset(openSftpByDefault, onChange, Persistence::SshSessionOptions{}.openSftpByDefault)
      }
    , remoteFavorites{
          language->getObserved("settings", "sessionSettings", "remoteFavoritesHelpText"),
          inputDialog,
          onChange,
          valueReset(remoteFavorites, onChange, Persistence::SshSessionOptions{}.remoteFavorites)
      }
    , sshOptions{onChange, inputDialog, multiInputDialog}
    , sftpOptions{onChange}
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