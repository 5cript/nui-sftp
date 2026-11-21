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
    , server{
          connectionFactory_,
          miscellaneousFactory_,
          "ftpSessionOptions",
          RemoteServerSettings::defaultsOf(Persistence::FtpSessionOptions{}),
          onChange,
          inputDialog
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
          confirmedUsersReset(
              users, Persistence::FtpSessionOptions{}.users, "ftpSessionOptions", onChange, confirmDialog
          ),
          ftpUserLabel
      }
    , ftpOptions{factory.within({"ftpOptionsGroupName"}), onChange}
{
    showDefaultPort();
}

void FtpSessionOptions::applyToState(Persistence::FtpSessionOptions& state) const
{
    server.applyToState(state);
    assignIfValid(state.tlsMode, tlsMode);
    state.pinnedPublicKeySha256 = nonEmpty(pinnedPublicKeySha256.value());
    state.users = users.value();
    ftpOptions.applyToState(state.ftpOptions.value());

    state.ftpOptions.ref(*ftpOptions.groupKey);
}

void FtpSessionOptions::loadFromState(Persistence::FtpSessionOptions const& state, bool loadRefs)
{
    server.loadFromState(state);
    tlsMode.value(state.tlsMode);
    showDefaultPort();
    pinnedPublicKeySha256.value(state.pinnedPublicKeySha256.value_or(std::string{}));
    users.value(state.users);
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
            fragment(server.host(), server.port(), tlsMode(), pinnedPublicKeySha256())
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpSessionOptions", "usersCategory"),
                .onChange = onChange},
            users()
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "sessionOptions", "miscellaneousCategory"),
                .onChange = onChange},
            fragment(server.remoteFavorites(), server.maxReconnectAttempts(), server.maxReconnectBackoffMs())
        )
    );
}

void FtpSessionOptions::showDefaultPort()
{
    server.port.fallback(Persistence::defaultFtpPort(tlsMode.value()));
}

std::function<void()> FtpSessionOptions::tlsModeChanged(std::function<void()> onChange)
{
    return [this, onChange = std::move(onChange)]()
    {
        showDefaultPort();
        onChange();
    };
}
