#pragma once

#include <events/app_event_context.hpp>
#include <events/app_wide_events.hpp>
#include <shared_data/theme.hpp>
#include <constants/persistence.hpp>

#include <persistence/state/state.hpp>

struct FrontendEvents : public AppWideEvents
{
    FrontendEvents()
        : AppWideEvents()
    {}

    Nui::Observed<std::string> onNewSession{};
    Nui::Observed<bool> onLayoutsChanged{false};
    Nui::Observed<bool> settingsOpen{false};
    Nui::Observed<Persistence::State> onSettingsChanged{};
    Nui::Observed<std::string> selectedTheme{std::string{Constants::defaultThemeName}};
    Nui::Observed<SharedData::DarkLightMode> darkLightMode{SharedData::DarkLightMode::System};
};