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

    /**
     * @brief Resets the users only after a confirmation, because a reset removes all of them.
     */
    auto confirmedUsersReset(auto& users, std::function<void()> onChange, ConfirmDialog& confirmDialog)
    {
        return [&users, onChange = std::move(onChange), &confirmDialog]()
        {
            confirmDialog.open({
                .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                .headerText = language->get("settings", "sessionOptions", "resetUsersConfirmHeader"),
                .text = language->get("settings", "sessionOptions", "resetUsersConfirmText"),
                .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                .onClose = [&users, onChange](std::optional<ConfirmDialog::Button> button)
                {
                    if (button != ConfirmDialog::Button::Ok)
                        return;
                    users.value(Persistence::SshSessionOptions{}.users);
                    onChange();
                },
            });
        };
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
    , host{
          connectionFactory_.identity({"sessionOptions", "host"}),
          onChange,
          valueReset(host, onChange, Persistence::SshSessionOptions{}.host)
      }
    , port{
          connectionFactory_.identity({"sessionOptions", "port"}),
          onChange,
          nulloptReset(port, onChange),
          {
            .minValue = 1,
            .maxValue = 65535,
          }
      }
    , users{
          usersFactory_.identity({"sessionOptions", "users"}),
          usersFactory_,
          onChange,
          confirmedUsersReset(users, onChange, confirmDialog),
          sshUserLabel
      }
    , openSftpByDefault{
          miscellaneousFactory_.identity({"sessionOptions", "openSftpByDefault"}),
          onChange,
          valueReset(openSftpByDefault, onChange, Persistence::SshSessionOptions{}.openSftpByDefault)
      }
    , remoteFavorites{
          miscellaneousFactory_.identity({"sessionOptions", "remoteFavorites"}),
          inputDialog,
          onChange,
          valueReset(remoteFavorites, onChange, Persistence::SshSessionOptions{}.remoteFavorites)
      }
    , maxReconnectAttempts{
          miscellaneousFactory_.identity({"sessionOptions", "maxReconnectAttempts"}),
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
          miscellaneousFactory_.identity({"sessionOptions", "maxReconnectBackoffMs"}),
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
    state.users = users.value();
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
    users.value(state.users);
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

Nui::ElementRenderer SshSessionOptions::renderServerSettings(std::function<void()> const& onChange)
{
    using Nui::Elements::fragment;

    return fragment(
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "connectionCategory"),
                .onChange = onChange},
            fragment(host(), port())
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "usersCategory"), .onChange = onChange},
            users()
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "miscellaneousCategory"),
                .onChange = onChange},
            fragment(openSftpByDefault(), remoteFavorites(), maxReconnectAttempts(), maxReconnectBackoffMs())
        )
    );
}
