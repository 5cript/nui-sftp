#include <frontend/settings/ftp_session_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/settings/subgroup.hpp>
#include <utility/language.hpp>

#include <log/log.hpp>

#include <nui/frontend/elements/fragment.hpp>
#include <nui/event_system/event_context.hpp>
#include <nui/frontend/val.hpp>
#include <nui/rpc.hpp>
#include <fmt/format.h>

namespace
{
    SettingFactory serverFactory(SettingFactory const& factory)
    {
        return factory.within({"sessionOptions", "basicSettings"})
            .within({"sessionOptions", "ftpSessionServerOptions"});
    }

    /**
     * @brief @p reply of "CertificateTrust::trustedKey" as the setting shows it: the key, marked when it is trusted
     *        only until the application ends. Empty without a key.
     */
    std::string trustedKeyText(Nui::val const& reply)
    {
        const auto key = reply["publicKeySha256"];
        if (!key.isString())
            return {};
        auto text = key.as<std::string>();
        if (text.empty() || reply["lasting"].as<bool>())
            return text;
        return fmt::format(
            fmt::runtime(language->get("settings", "ftpSessionOptions", "trustedPublicKeyThisRun")), text
        );
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
          serverChanged(onChange),
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
    , trustedPublicKey{
          connectionFactory_.identity({"ftpSessionOptions", "trustedPublicKey"}),
          onChange,
          [this]()
          {
              forgetTrustedKey();
          }
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
    state.users = users.value();
    ftpOptions.applyToState(state.ftpOptions.value());

    state.ftpOptions.ref(*ftpOptions.groupKey);
}

void FtpSessionOptions::loadFromState(Persistence::FtpSessionOptions const& state, bool loadRefs)
{
    server.loadFromState(state);
    tlsMode.value(state.tlsMode);
    showDefaultPort();
    showTrustedKey();
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
            fragment(server.host(), server.port(), tlsMode(), trustedPublicKey())
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
        showTrustedKey();
        onChange();
    };
}

std::function<void()> FtpSessionOptions::serverChanged(std::function<void()> onChange)
{
    return [this, onChange = std::move(onChange)]()
    {
        showTrustedKey();
        onChange();
    };
}

int FtpSessionOptions::effectivePort() const
{
    return server.port.value().value_or(Persistence::defaultFtpPort(tlsMode.value()));
}

void FtpSessionOptions::showTrustedKey()
{
    const auto host = server.host.value();
    const auto port = effectivePort();
    Nui::RpcClient::callWithBackChannel(
        "CertificateTrust::trustedKey",
        lifetime_.guarded(
            [this, host, port](Nui::val reply)
            {
                // The host or the port was edited meanwhile, its own reply follows.
                if (host != server.host.value() || port != effectivePort())
                    return;
                trustedPublicKey.value(trustedKeyText(reply));
                Nui::globalEventContext.executeActiveEventsImmediately();
            }
        ),
        host,
        port
    );
}

void FtpSessionOptions::forgetTrustedKey()
{
    Nui::RpcClient::callWithBackChannel(
        "CertificateTrust::forget",
        lifetime_.guarded(
            [this](Nui::val reply)
            {
                if (reply.hasOwnProperty("error"))
                    Log::error("The trusted key could not be forgotten: {}", reply["error"].as<std::string>());
                showTrustedKey();
            }
        ),
        server.host.value(),
        effectivePort()
    );
}
