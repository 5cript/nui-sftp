#include <frontend/settings/remote_server_settings.hpp>

#include <frontend/settings/nullopt_reset.hpp>

RemoteServerSettings::RemoteServerSettings(
    SettingFactory const& connectionFactory,
    SettingFactory const& miscellaneousFactory,
    std::string const& languageSection,
    Defaults const& defaults,
    std::function<void()> const& onChange,
    InputDialog& inputDialog
)
    : host{
          connectionFactory.identity({languageSection, "host"}),
          onChange,
          valueReset(host, onChange, defaults.host)
      }
    , port{
          connectionFactory.identity({languageSection, "port"}),
          onChange,
          nulloptReset(port, onChange),
          {
              .minValue = 1,
              .maxValue = 65535,
          }
      }
    , remoteFavorites{
          miscellaneousFactory.identity({languageSection, "remoteFavorites"}),
          inputDialog,
          onChange,
          valueReset(remoteFavorites, onChange, defaults.remoteFavorites)
      }
    , maxReconnectAttempts{
          miscellaneousFactory.identity({languageSection, "maxReconnectAttempts"}),
          onChange,
          valueReset(maxReconnectAttempts, onChange, defaults.maxReconnectAttempts),
          {
              .minValue = -1,
              .maxValue = 1000,
          }
      }
    , maxReconnectBackoffMs{
          miscellaneousFactory.identity({languageSection, "maxReconnectBackoffMs"}),
          onChange,
          valueReset(maxReconnectBackoffMs, onChange, defaults.maxReconnectBackoffMs),
          {
              .minValue = 1000,
              .maxValue = 600000,
          }
      }
{}
