#pragma once

#include <events/app_event_context.hpp>
#include <events/app_wide_events.hpp>

#include <persistence/state/state.hpp>

struct FrontendEvents : public AppWideEvents
{
    Nui::Observed<std::string> onNewSession{};
    Nui::Observed<bool> onLayoutsChanged{false};
    Nui::Observed<bool> settingsOpen{false};
    Nui::Observed<Persistence::State> onSettingsChanged{};
};