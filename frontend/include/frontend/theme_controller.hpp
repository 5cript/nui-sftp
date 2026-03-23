#pragma once

#include <nui/event_system/listen.hpp>
#include <frontend/events/frontend_events.hpp>
#include <shared_data/theme.hpp>

#include <optional>
#include <string>

class ThemeController
{
  public:
    ThemeController(FrontendEvents& events);

    SharedData::DarkLightMode getPreferredMode();
    void applyMode(SharedData::DarkLightMode setting);
    SharedData::DarkLightMode getAppliedMode() const;

    // Convenience — wraps events_->themeName assignment + sync,
    // same pattern as applyMode(). Pass empty string to unload.
    void applyThemeName(const std::string& name);

    std::optional<std::string> getActiveCustomTheme() const;

  private:
    void applyModeImpl(SharedData::DarkLightMode setting);
    void applyThemeNameImpl(const std::string& name);
    void unloadCustomThemeImpl();

    static std::string sanitizeThemeName(const std::string& name);

  private:
    FrontendEvents* events_;
    Nui::ListenRemover<Nui::Observed<SharedData::DarkLightMode>> themeChangeListener_;
    Nui::ListenRemover<Nui::Observed<std::string>> themeNameListener_;
    std::optional<std::string> activeCustomTheme_;
};