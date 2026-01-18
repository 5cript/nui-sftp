#include <frontend/settings/ssh_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>

using namespace std::string_literals;

SshSessionOptions::SshSessionOptions(std::function<void()> const& onChange)
    : host{
          language->getObserved("settings", "sessionSettings", "host"),
          onChange,
          valueReset(host, onChange, Persistence::SshSessionOptions{}.host)
      }
    , port{
          language->getObserved("settings", "sessionSettings", "port"),
          onChange,
          nulloptReset(port, onChange)
      }
    , user{
          language->getObserved("settings", "sessionSettings", "user"),
          onChange,
          nulloptReset(user, onChange)
      }
    , sshKey{
          language->getObserved("settings", "sessionSettings", "sshKey"),
          onChange,
          nulloptReset(sshKey, onChange)
      }
    , openSftpByDefault{
          language->getObserved("settings", "sessionSettings", "openSftpByDefault"),
          onChange,
          valueReset(openSftpByDefault, onChange, Persistence::SshSessionOptions{}.openSftpByDefault)
      }
    , sshOptions{onChange}
    , sftpOptions{onChange}
{}

void SshSessionOptions::applyToState(Persistence::SshSessionOptions& state) const
{
    state.host = host.value();
    state.port = port.value();
    state.user = user.value();
    state.sshKey = sshKey.value();
    state.openSftpByDefault = openSftpByDefault.value();
    sshOptions.applyToState(state.sshOptions.value());
    sftpOptions.applyToState(state.sftpOptions.value());
}

void SshSessionOptions::loadFromState(Persistence::SshSessionOptions const& state)
{
    host.value(state.host);
    port.value(state.port);
    user.value(state.user);
    sshKey.value(state.sshKey);
    openSftpByDefault.value(state.openSftpByDefault);
    sshOptions.loadFromState(state.sshOptions.value());
    sftpOptions.loadFromState(state.sftpOptions.value());
}

void SshSessionOptions::assumeDefaultsFrom(Persistence::SshSessionOptions const& state)
{
    sshOptions.assumeDefaultsFrom(state.sshOptions.value());
    sftpOptions.assumeDefaultsFrom(state.sftpOptions.value());
}