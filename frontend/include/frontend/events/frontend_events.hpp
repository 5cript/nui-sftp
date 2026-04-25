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
    Nui::Observed<bool> licensesOpen{false};
    /// Opens settings and scrolls to the rendered element whose DOM id equals
    /// this string. Settings walks up from the element to find its
    /// [data-settings-section] ancestor and activates that section first, so
    /// the target is actually visible before the scroll.
    /// Consumers should use `requestOpenSettingsAtId()` rather than setting
    /// this directly.
    Nui::Observed<std::optional<std::string>> requestedSettingScrollId{};
    Nui::Observed<Persistence::State> onSettingsChanged{};
    Nui::Observed<std::string> selectedTheme{std::string{Constants::defaultThemeName}};
    Nui::Observed<SharedData::DarkLightMode> darkLightMode{SharedData::DarkLightMode::System};

    /// Open the settings dialog and scroll it to the element with the given
    /// DOM id. The id must be present on something inside a rendered settings
    /// section (see `addressableSetting()`).
    void requestOpenSettingsAtId(std::string htmlId)
    {
        settingsOpen = true;
        requestedSettingScrollId = std::move(htmlId);
    }
};