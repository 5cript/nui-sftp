#include <frontend/settings/ftp_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/settings/subgroup.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/fragment.hpp>

namespace
{
    SettingFactory serverFactory(SettingFactory const& factory)
    {
        return factory.within({"sessionOptions", "basicSettings"})
            .within({"sessionOptions", "ftpSessionServerOptions"});
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
                .headerText = language->get("settings", "ftpSessionOptions", "resetUsersConfirmHeader"),
                .text = language->get("settings", "ftpSessionOptions", "resetUsersConfirmText"),
                .buttons = ConfirmDialog::Button::Ok | ConfirmDialog::Button::Cancel,
                .onClose = [&users, onChange](std::optional<ConfirmDialog::Button> button)
                {
                    if (button != ConfirmDialog::Button::Ok)
                        return;
                    users.value(Persistence::FtpSessionOptions{}.users);
                    onChange();
                },
            });
        };
    }

    std::optional<std::string> nonEmpty(std::string const& text)
    {
        if (text.empty())
            return std::nullopt;
        return text;
    }
}

std::string ftpTlsModeLabel(Persistence::FtpTlsMode tlsMode)
{
    switch (tlsMode)
    {
        case Persistence::FtpTlsMode::Plain:
            return language->get("settings", "ftpSessionOptions", "tlsModePlain");
        case Persistence::FtpTlsMode::Explicit:
            return language->get("settings", "ftpSessionOptions", "tlsModeExplicit");
        case Persistence::FtpTlsMode::Implicit:
            return language->get("settings", "ftpSessionOptions", "tlsModeImplicit");
    }
    return "???";
}

FtpSessionOptions::FtpSessionOptions(
    SettingFactory const& factory,
    std::function<void()> const& onChange,
    ConfirmDialog& confirmDialog,
    InputDialog& inputDialog
)
    : connectionFactory_{serverFactory(factory).within({"sessionOptions", "connectionCategory"})}
    , usersFactory_{serverFactory(factory).within({"ftpSessionOptions", "usersCategory"})}
    , miscellaneousFactory_{serverFactory(factory).within({"sessionOptions", "miscellaneousCategory"})}
    , host{
          connectionFactory_.identity({"ftpSessionOptions", "host"}),
          onChange,
          valueReset(host, onChange, Persistence::FtpSessionOptions{}.host)
      }
    , port{
          connectionFactory_.identity({"ftpSessionOptions", "port"}),
          onChange,
          nulloptReset(port, onChange),
          {
              .minValue = 1,
              .maxValue = 65535,
          }
      }
    , tlsMode{
          {
              Persistence::FtpTlsMode::Explicit,
              Persistence::FtpTlsMode::Implicit,
              Persistence::FtpTlsMode::Plain,
          },
          connectionFactory_.identity({"ftpSessionOptions", "tlsMode"}),
          tlsModeChanged(onChange),
          valueReset(tlsMode, tlsModeChanged(onChange), Persistence::FtpSessionOptions{}.tlsMode),
          ftpTlsModeLabel
      }
    , pinnedPublicKeySha256{
          connectionFactory_.identity({"ftpSessionOptions", "pinnedPublicKeySha256"}),
          onChange,
          valueReset(pinnedPublicKeySha256, onChange, std::string{})
      }
    , users{
          usersFactory_.identity({"ftpSessionOptions", "users"}),
          usersFactory_,
          onChange,
          confirmedUsersReset(users, onChange, confirmDialog),
          ftpUserLabel
      }
    , remoteFavorites{
          miscellaneousFactory_.identity({"ftpSessionOptions", "remoteFavorites"}),
          inputDialog,
          onChange,
          valueReset(remoteFavorites, onChange, Persistence::FtpSessionOptions{}.remoteFavorites)
      }
    , maxReconnectAttempts{
          miscellaneousFactory_.identity({"ftpSessionOptions", "maxReconnectAttempts"}),
          onChange,
          valueReset(maxReconnectAttempts, onChange, Persistence::FtpSessionOptions{}.maxReconnectAttempts),
          {
              .minValue = -1,
              .maxValue = 1000,
          }
      }
    , maxReconnectBackoffMs{
          miscellaneousFactory_.identity({"ftpSessionOptions", "maxReconnectBackoffMs"}),
          onChange,
          valueReset(maxReconnectBackoffMs, onChange, Persistence::FtpSessionOptions{}.maxReconnectBackoffMs),
          {
              .minValue = 1000,
              .maxValue = 600000,
          }
      }
    , ftpOptions{factory.within({"ftpOptionsGroupName"}), onChange}
{
    showDefaultPort();
}

void FtpSessionOptions::applyToState(Persistence::FtpSessionOptions& state) const
{
    assignIfValid(state.host, host);
    assignIfValid(state.port, port);
    assignIfValid(state.tlsMode, tlsMode);
    state.pinnedPublicKeySha256 = nonEmpty(pinnedPublicKeySha256.value());
    state.users = users.value();
    state.remoteFavorites = remoteFavorites.value();
    assignIfValid(state.maxReconnectAttempts, maxReconnectAttempts);
    assignIfValid(state.maxReconnectBackoffMs, maxReconnectBackoffMs);
    ftpOptions.applyToState(state.ftpOptions.value());

    state.ftpOptions.ref(*ftpOptions.groupKey);
}

void FtpSessionOptions::loadFromState(Persistence::FtpSessionOptions const& state, bool loadRefs)
{
    host.value(state.host);
    port.value(state.port);
    tlsMode.value(state.tlsMode);
    showDefaultPort();
    pinnedPublicKeySha256.value(state.pinnedPublicKeySha256.value_or(std::string{}));
    users.value(state.users);
    remoteFavorites.value(state.remoteFavorites);
    maxReconnectAttempts.value(state.maxReconnectAttempts);
    maxReconnectBackoffMs.value(state.maxReconnectBackoffMs);
    ftpOptions.loadFromState(state.ftpOptions.value(), loadRefs);

    if (loadRefs)
    {
        ftpOptions.groupKey =
            state.ftpOptions.hasReference() ? std::optional<std::string>{state.ftpOptions.ref()} : std::nullopt;
    }
}

void FtpSessionOptions::assumeDefaultsFrom(Persistence::FtpSessionOptions const& state)
{
    ftpOptions.assumeDefaultsFrom(state.ftpOptions.value());
}

Nui::ElementRenderer FtpSessionOptions::renderServerSettings(std::function<void()> const& onChange)
{
    using Nui::Elements::fragment;

    return fragment(
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "connectionCategory"),
                .onChange = onChange},
            fragment(host(), port(), tlsMode(), pinnedPublicKeySha256())
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpSessionOptions", "usersCategory"),
                .onChange = onChange},
            users()
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "miscellaneousCategory"),
                .onChange = onChange},
            fragment(remoteFavorites(), maxReconnectAttempts(), maxReconnectBackoffMs())
        )
    );
}

void FtpSessionOptions::showDefaultPort()
{
    port.fallback(Persistence::defaultFtpPort(tlsMode.value()));
}

std::function<void()> FtpSessionOptions::tlsModeChanged(std::function<void()> onChange)
{
    return [this, onChange = std::move(onChange)]()
    {
        showDefaultPort();
        onChange();
    };
}
