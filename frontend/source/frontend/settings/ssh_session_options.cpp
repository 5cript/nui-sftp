#include <frontend/settings/ssh_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/settings/subgroup.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/fragment.hpp>

using namespace std::string_literals;

namespace
{
    SettingFactory serverFactory(SettingFactory const& factory)
    {
        return factory.within({"sessionOptions", "basicSettings"})
            .within({"sessionOptions", "sshSessionServerOptions"});
    }
}

SshSessionOptions::SshSessionOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    ConfirmDialog& confirmDialog,
    InputDialog& inputDialog,
    MultiInputDialog& multiInputDialog
)
    : connectionFactory_{serverFactory(factory).within({"sessionOptions", "connectionCategory"})}
    , usersFactory_{serverFactory(factory).within({"sessionOptions", "usersCategory"})}
    , miscellaneousFactory_{serverFactory(factory).within({"sessionOptions", "miscellaneousCategory"})}
    , server{
          connectionFactory_,
          miscellaneousFactory_,
          "sessionOptions",
          RemoteServerSettings::defaultsOf(Persistence::SshSessionOptions{}),
          onChange,
          inputDialog
      }
    , users{
          usersFactory_.identity({"sessionOptions", "users"}),
          usersFactory_,
          onChange,
          confirmedUsersReset(
              users, Persistence::SshSessionOptions{}.users, "sessionOptions", onChange, confirmDialog
          ),
          sshUserLabel
      }
    , openSftpByDefault{
          miscellaneousFactory_.identity({"sessionOptions", "openSftpByDefault"}),
          onChange,
          valueReset(openSftpByDefault, onChange, Persistence::SshSessionOptions{}.openSftpByDefault)
      }
    , sshOptions{factory.within({"sshOptionsGroupName"}), onChange, inputDialog, multiInputDialog}
{}

void SshSessionOptions::applyToState(Persistence::SshSessionOptions& state) const
{
    server.applyToState(state);
    state.users = users.value();
    assignIfValid(state.openSftpByDefault, openSftpByDefault);
    sshOptions.applyToState(state.sshOptions.value());

    state.sshOptions.ref(*sshOptions.groupKey);
}

void SshSessionOptions::loadFromState(Persistence::SshSessionOptions const& state, bool loadRefs)
{
    server.loadFromState(state);
    users.value(state.users);
    openSftpByDefault.value(state.openSftpByDefault);
    sshOptions.loadFromState(state.sshOptions.value(), loadRefs);

    if (loadRefs)
    {
        sshOptions.groupKey =
            state.sshOptions.hasReference() ? std::optional<std::string>{state.sshOptions.ref()} : std::nullopt;
    }
}

void SshSessionOptions::assumeDefaultsFrom(Persistence::SshSessionOptions const& state)
{
    sshOptions.assumeDefaultsFrom(state.sshOptions.value());
}

Nui::ElementRenderer SshSessionOptions::renderServerSettings(std::function<void()> const& onChange)
{
    using Nui::Elements::fragment;

    return fragment(
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "connectionCategory"),
                .onChange = onChange},
            fragment(server.host(), server.port())
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "usersCategory"), .onChange = onChange},
            users()
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "miscellaneousCategory"),
                .onChange = onChange},
            fragment(
                openSftpByDefault(),
                server.remoteFavorites(),
                server.maxReconnectAttempts(),
                server.maxReconnectBackoffMs()
            )
        )
    );
}
