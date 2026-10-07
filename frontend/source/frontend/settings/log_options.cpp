#include <frontend/settings/log_options.hpp>

#include <frontend/svgs/activity-items.hpp>
#include <frontend/svgs/zoom-in.hpp>
#include <frontend/svgs/information.hpp>
#include <frontend/svgs/alert.hpp>
#include <frontend/svgs/error.hpp>
#include <frontend/svgs/incident.hpp>
#include <frontend/svgs/hide.hpp>

LogOptions::LogOptions(SettingFactory const& factory, std::function<void()> const& onChange)
    : logLevel{
          {
              Log::Level::Trace,
              Log::Level::Debug,
              Log::Level::Info,
              Log::Level::Warning,
              Log::Level::Error,
              Log::Level::Critical,
              Log::Level::Off,
          },
          factory.identity({"logOptions", "logLevel"}),
          onChange,
          [this, onChange]()
          {
              logLevel.value(Persistence::LogOptions{}.logLevel);
              onChange();
          },
          [](Log::Level const& level)
          {
              return Utility::enumToString<Log::Level>(level);
          },
          [](Log::Level const& level) -> Nui::ElementRenderer
          {
              switch (level)
              {
                  case Log::Level::Trace:
                      return GeneratedSvgs::activityitems();
                  case Log::Level::Debug:
                      return GeneratedSvgs::zoomin();
                  case Log::Level::Info:
                      return GeneratedSvgs::information();
                  case Log::Level::Warning:
                      return GeneratedSvgs::alert();
                  case Log::Level::Error:
                      return GeneratedSvgs::error();
                  case Log::Level::Critical:
                      return GeneratedSvgs::incident();
                  case Log::Level::Off:
                      return GeneratedSvgs::hide();
                  default:
                      return Nui::nil();
              }
          }
      }
    , logDirectory{
          factory.identity({"logOptions", "logDirectory"}),
          [this]()
          {
              onChange_();
          },
          [this]()
          {
              logDirectory.value(Persistence::LogOptions{}.logDirectory);
              onChange_();
          }
      }
    , disableFileLogging{
          factory.identity({"logOptions", "disableFileLogging"}),
          [this]()
          {
              onChange_();
          },
          [this]()
          {
              disableFileLogging.value(Persistence::LogOptions{}.disableFileLogging);
              onChange_();
          }
      }
    , onChange_{onChange}
    , logLevelListener_{Nui::smartListen(
          logLevel.state(),
          [](Log::Level const& level)
          {
              Log::setLevel(level);
          }
      )}
{}

void LogOptions::applyToState(Persistence::LogOptions& state) const
{
    state.logLevel = logLevel.value();
    state.logDirectory = logDirectory.value();
    state.disableFileLogging = disableFileLogging.value();
}
void LogOptions::loadFromState(Persistence::LogOptions const& state)
{
    logLevel.value(state.logLevel);
    logDirectory.value(state.logDirectory);
    disableFileLogging.value(state.disableFileLogging);
}
void LogOptions::assumeDefaultsFrom(Persistence::LogOptions const& state)
{
    logLevel.inheritValue(state.logLevel);
    logDirectory.inheritValue(state.logDirectory);
    disableFileLogging.inheritValue(state.disableFileLogging);
}
Nui::ElementRenderer LogOptions::render()
{
    using namespace Nui::Elements;

    return fragment(
        logLevel(),
        logDirectory(),
        disableFileLogging()
    );
}