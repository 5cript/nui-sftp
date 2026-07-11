#include <frontend/settings/history_options.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/setting_helper.hpp>
#include <utility/language.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

namespace
{
    /// The enum names are what lands in the state file; the user sees the translated ones.
    std::string captureModeLabel(Persistence::HistoryCaptureMode mode)
    {
        switch (mode)
        {
            case Persistence::HistoryCaptureMode::off:
                return language->get("settings", "historyOptions", "captureModeOff");
            case Persistence::HistoryCaptureMode::simple:
                return language->get("settings", "historyOptions", "captureModeSimple");
            case Persistence::HistoryCaptureMode::smart:
                return language->get("settings", "historyOptions", "captureModeSmart");
        }
        return {};
    }
}

HistoryOptions::HistoryOptions(std::function<void()> const& onChange)
    : captureMode{
          std::vector<Persistence::HistoryCaptureMode>{
              Persistence::HistoryCaptureMode::off,
              Persistence::HistoryCaptureMode::simple,
              Persistence::HistoryCaptureMode::smart,
          },
          language->getObserved("settings", "historyOptions", "captureModeHelpText"),
          onChange,
          nulloptReset(captureMode, onChange),
          [](Persistence::HistoryCaptureMode const& mode) {
              return captureModeLabel(mode);
          },
      }
{}

void HistoryOptions::applyToState(Persistence::HistoryOptions& state) const
{
    assignIfValid(state.captureMode, captureMode);
}

void HistoryOptions::loadFromState(Persistence::HistoryOptions const& state)
{
    captureMode.value(state.captureMode);
}

void HistoryOptions::assumeDefaultsFrom(Persistence::HistoryOptions const& state)
{
    captureMode.inherit(state.captureMode);
}

Nui::ElementRenderer HistoryOptions::render()
{
    using namespace Nui::Elements;

    return fragment(captureMode(language->getObserved("settings", "historyOptions", "captureMode")));
}
