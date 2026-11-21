#include <frontend/settings/ftp_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <frontend/settings/subgroup.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements/fragment.hpp>

namespace
{
    std::string extendedPassiveLabel(Persistence::FtpExtendedPassive extendedPassive)
    {
        switch (extendedPassive)
        {
            case Persistence::FtpExtendedPassive::Automatic:
                return language->get("settings", "ftpOptions", "extendedPassiveAutomatic");
            case Persistence::FtpExtendedPassive::Always:
                return language->get("settings", "ftpOptions", "extendedPassiveAlways");
            case Persistence::FtpExtendedPassive::Never:
                return language->get("settings", "ftpOptions", "extendedPassiveNever");
        }
        return "???";
    }

    std::string tlsVersionLabel(Persistence::FtpTlsVersion tlsVersion)
    {
        switch (tlsVersion)
        {
            case Persistence::FtpTlsVersion::Tls12:
                return "TLS 1.2";
            case Persistence::FtpTlsVersion::Tls13:
                return "TLS 1.3";
        }
        return "???";
    }
}

FtpOptions::FtpOptions(SettingFactory const& factory, std::function<void()> const& onChange)
    : timeoutsFactory_{factory.forGroup(groupKey).within({"ftpOptions", "timeoutsSubgroupTitle"})}
    , connectionsFactory_{factory.forGroup(groupKey).within({"ftpOptions", "connectionsSubgroupTitle"})}
    , keepAliveFactory_{factory.forGroup(groupKey).within({"ftpOptions", "keepAliveSubgroupTitle"})}
    , securityFactory_{factory.forGroup(groupKey).within({"ftpOptions", "securitySubgroupTitle"})}
    , connectTimeoutSeconds{
          timeoutsFactory_.identity({"ftpOptions", "connectTimeoutSeconds"}),
          onChange,
          nulloptReset(connectTimeoutSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 600,
          }
      }
    , commandTimeoutSeconds{
          timeoutsFactory_.identity({"ftpOptions", "commandTimeoutSeconds"}),
          onChange,
          nulloptReset(commandTimeoutSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 600,
          }
      }
    , dataInactivityTimeoutSeconds{
          timeoutsFactory_.identity({"ftpOptions", "dataInactivityTimeoutSeconds"}),
          onChange,
          nulloptReset(dataInactivityTimeoutSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 3600,
          }
      }
    , transferReplyTimeoutSeconds{
          timeoutsFactory_.identity({"ftpOptions", "transferReplyTimeoutSeconds"}),
          onChange,
          nulloptReset(transferReplyTimeoutSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 3600,
          }
      }
    , maxConnections{
          connectionsFactory_.identity({"ftpOptions", "maxConnections"}),
          onChange,
          nulloptReset(maxConnections, onChange),
          {
              .minValue = 2,
              .maxValue = 32,
          }
      }
    , extendedPassive{
          {
              Persistence::FtpExtendedPassive::Automatic,
              Persistence::FtpExtendedPassive::Always,
              Persistence::FtpExtendedPassive::Never,
          },
          connectionsFactory_.identity({"ftpOptions", "extendedPassive"}),
          onChange,
          nulloptReset(extendedPassive, onChange),
          extendedPassiveLabel
      }
    , trustPassiveAddress{
          connectionsFactory_.identity({"ftpOptions", "trustPassiveAddress"}),
          onChange,
          nulloptReset(trustPassiveAddress, onChange),
      }
    , keepIdleAlive{
          keepAliveFactory_.identity({"ftpOptions", "keepIdleAlive"}),
          onChange,
          nulloptReset(keepIdleAlive, onChange),
      }
    , idleNoopIntervalSeconds{
          keepAliveFactory_.identity({"ftpOptions", "idleNoopIntervalSeconds"}),
          onChange,
          nulloptReset(idleNoopIntervalSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 3600,
          }
      }
    , keepaliveIdleSeconds{
          keepAliveFactory_.identity({"ftpOptions", "keepaliveIdleSeconds"}),
          onChange,
          nulloptReset(keepaliveIdleSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 7200,
          }
      }
    , keepaliveIntervalSeconds{
          keepAliveFactory_.identity({"ftpOptions", "keepaliveIntervalSeconds"}),
          onChange,
          nulloptReset(keepaliveIntervalSeconds, onChange),
          {
              .minValue = 1,
              .maxValue = 600,
          }
      }
    , keepaliveCount{
          keepAliveFactory_.identity({"ftpOptions", "keepaliveCount"}),
          onChange,
          nulloptReset(keepaliveCount, onChange),
          {
              .minValue = 1,
              .maxValue = 100,
          }
      }
    , minimumTlsVersion{
          {
              Persistence::FtpTlsVersion::Tls12,
              Persistence::FtpTlsVersion::Tls13,
          },
          securityFactory_.identity({"ftpOptions", "minimumTlsVersion"}),
          onChange,
          nulloptReset(minimumTlsVersion, onChange),
          tlsVersionLabel
      }
    , onChange_{onChange}
{}

void FtpOptions::applyToState(Persistence::FtpOptions& state) const
{
    assignIfValid(state.connectTimeoutSeconds, connectTimeoutSeconds);
    assignIfValid(state.commandTimeoutSeconds, commandTimeoutSeconds);
    assignIfValid(state.dataInactivityTimeoutSeconds, dataInactivityTimeoutSeconds);
    assignIfValid(state.transferReplyTimeoutSeconds, transferReplyTimeoutSeconds);
    assignIfValid(state.maxConnections, maxConnections);
    assignIfValid(state.extendedPassive, extendedPassive);
    assignIfValid(state.trustPassiveAddress, trustPassiveAddress);
    assignIfValid(state.keepIdleAlive, keepIdleAlive);
    assignIfValid(state.idleNoopIntervalSeconds, idleNoopIntervalSeconds);
    assignIfValid(state.keepaliveIdleSeconds, keepaliveIdleSeconds);
    assignIfValid(state.keepaliveIntervalSeconds, keepaliveIntervalSeconds);
    assignIfValid(state.keepaliveCount, keepaliveCount);
    assignIfValid(state.minimumTlsVersion, minimumTlsVersion);
}

void FtpOptions::loadFromState(Persistence::FtpOptions const& state, bool)
{
    connectTimeoutSeconds.value(state.connectTimeoutSeconds);
    commandTimeoutSeconds.value(state.commandTimeoutSeconds);
    dataInactivityTimeoutSeconds.value(state.dataInactivityTimeoutSeconds);
    transferReplyTimeoutSeconds.value(state.transferReplyTimeoutSeconds);
    maxConnections.value(state.maxConnections);
    extendedPassive.value(state.extendedPassive);
    trustPassiveAddress.value(state.trustPassiveAddress);
    keepIdleAlive.value(state.keepIdleAlive);
    idleNoopIntervalSeconds.value(state.idleNoopIntervalSeconds);
    keepaliveIdleSeconds.value(state.keepaliveIdleSeconds);
    keepaliveIntervalSeconds.value(state.keepaliveIntervalSeconds);
    keepaliveCount.value(state.keepaliveCount);
    minimumTlsVersion.value(state.minimumTlsVersion);
}

void FtpOptions::assumeDefaultsFrom(Persistence::FtpOptions const& state)
{
    connectTimeoutSeconds.inherit(state.connectTimeoutSeconds);
    commandTimeoutSeconds.inherit(state.commandTimeoutSeconds);
    dataInactivityTimeoutSeconds.inherit(state.dataInactivityTimeoutSeconds);
    transferReplyTimeoutSeconds.inherit(state.transferReplyTimeoutSeconds);
    maxConnections.inherit(state.maxConnections);
    extendedPassive.inherit(state.extendedPassive);
    trustPassiveAddress.inherit(state.trustPassiveAddress);
    keepIdleAlive.inherit(state.keepIdleAlive);
    idleNoopIntervalSeconds.inherit(state.idleNoopIntervalSeconds);
    keepaliveIdleSeconds.inherit(state.keepaliveIdleSeconds);
    keepaliveIntervalSeconds.inherit(state.keepaliveIntervalSeconds);
    keepaliveCount.inherit(state.keepaliveCount);
    minimumTlsVersion.inherit(state.minimumTlsVersion);
}

Nui::ElementRenderer FtpOptions::render()
{
    using Nui::Elements::fragment;

    return fragment(
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpOptions", "timeoutsSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                connectTimeoutSeconds(),
                commandTimeoutSeconds(),
                dataInactivityTimeoutSeconds(),
                transferReplyTimeoutSeconds()
            )
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpOptions", "connectionsSubgroupTitle"),
                .onChange = onChange_},
            fragment(maxConnections(), extendedPassive(), trustPassiveAddress())
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpOptions", "keepAliveSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                keepIdleAlive(),
                idleNoopIntervalSeconds(),
                keepaliveIdleSeconds(),
                keepaliveIntervalSeconds(),
                keepaliveCount()
            )
        ),
        subgroup(
            {.groupTitle = language->getObserved("settings", "ftpOptions", "securitySubgroupTitle"),
                .onChange = onChange_},
            minimumTlsVersion()
        )
    );
}
