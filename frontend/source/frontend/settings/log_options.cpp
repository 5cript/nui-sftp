#include <frontend/settings/log_options.hpp>

#include <svgs/activity-items.hpp>
#include <svgs/zoom-in.hpp>
#include <svgs/information.hpp>
#include <svgs/alert.hpp>
#include <svgs/error.hpp>
#include <svgs/incident.hpp>
#include <svgs/hide.hpp>

LogOptions::LogOptions(std::function<void()> const& onChange)
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
          language->getObserved("settings", "logOptions", "logLevelHelpText"),
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
          language->getObserved("settings", "logOptions", "logDirectoryHelpText"),
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
          language->getObserved("settings", "logOptions", "disableFileLoggingHelpText"),
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
        logLevel(language->getObserved("settings", "logOptions", "logLevel")),
        logDirectory(language->getObserved("settings", "logOptions", "logDirectory")),
        disableFileLogging(language->getObserved("settings", "logOptions", "disableFileLogging"))
    );
}