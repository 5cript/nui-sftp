#include <frontend/theme_controller.hpp>
#include <nui/frontend/val.hpp>
#include <nui/frontend/api/console.hpp>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ThemeController::ThemeController(FrontendEvents& events)
    : events_{&events}
    , themeChangeListener_(
          Nui::smartListen(
              events.darkLightMode,
              [this](SharedData::DarkLightMode setting)
              {
                  applyModeImpl(setting);
              }
          )
      )
    , themeNameListener_(
          Nui::smartListen(
              events.themeName,
              [this](const std::string& name)
              {
                  applyThemeNameImpl(name);
              }
          )
      )
{
    applyModeImpl(getPreferredMode());
    applyThemeNameImpl(events.themeName.value());
}

// ---------------------------------------------------------------------------
// Light / dark mode
// ---------------------------------------------------------------------------

SharedData::DarkLightMode ThemeController::getPreferredMode()
{
    using namespace std::string_literals;

    const auto isLightMode =
        Nui::val::global().call<Nui::val>("matchMedia", "(prefers-color-scheme: light)"s)["matches"].as<bool>();

    return isLightMode ? SharedData::DarkLightMode::Light : SharedData::DarkLightMode::Dark;
}

void ThemeController::applyModeImpl(SharedData::DarkLightMode setting)
{
    const auto root = Nui::val::global("document")["documentElement"];
    if (root.isUndefined())
    {
        Nui::WebApi::Console::warn("Cannot apply light/dark: document root is undefined.");
        return;
    }

    const std::string themeValue = [&]() -> std::string
    {
        switch (setting)
        {
            case SharedData::DarkLightMode::System:
                return getPreferredMode() == SharedData::DarkLightMode::Dark ? "dark" : "light";
            case SharedData::DarkLightMode::Dark:
                return "dark";
            case SharedData::DarkLightMode::Light:
                return "light";
        }
        return "failed";
    }();

    Nui::WebApi::Console::log("Applying theme: " + themeValue);
    Nui::val::global("document")["documentElement"]["dataset"].set("theme", themeValue);
}

void ThemeController::applyMode(SharedData::DarkLightMode setting)
{
    events_->darkLightMode = setting;
    events_->darkLightMode.eventContext().sync();
}

SharedData::DarkLightMode ThemeController::getAppliedMode() const
{
    const auto root = Nui::val::global("document")["documentElement"];
    if (root.isUndefined())
        return SharedData::DarkLightMode::System;

    if (root["dataset"].hasOwnProperty("theme"))
    {
        const auto theme = root["dataset"]["theme"].as<std::string>();
        if (theme == "dark")
            return SharedData::DarkLightMode::Dark;
        if (theme == "light")
            return SharedData::DarkLightMode::Light;
    }
    return SharedData::DarkLightMode::System;
}

// ---------------------------------------------------------------------------
// Custom theme name
// ---------------------------------------------------------------------------

std::string ThemeController::sanitizeThemeName(const std::string& name)
{
    if (name.empty())
        return {};

    for (char c : name)
    {
        if (c == '/' || c == '\\' || c == '.' || c == ':' || c == '?' || c == '#' || c == '%' || c == '\0')
        {
            Nui::WebApi::Console::warn("ThemeController: rejected character in theme name.");
            return {};
        }
    }

    if (name.find("..") != std::string::npos)
        return {};

    return name;
}

void ThemeController::unloadCustomThemeImpl()
{
    using namespace std::string_literals;

    auto doc = Nui::val::global("document");
    auto existing = doc.call<Nui::val>("getElementById", "nui-custom-theme-link"s);
    if (!existing.isNull() && !existing.isUndefined())
        existing["parentNode"].call<void>("removeChild", existing);

    doc["documentElement"]["dataset"].delete_("customTheme");
    activeCustomTheme_ = std::nullopt;
}

void ThemeController::applyThemeNameImpl(const std::string& name)
{
    using namespace std::string_literals;

    // "default" (the initial value) or empty string both mean: no custom theme
    if (name.empty() || name == "default")
    {
        unloadCustomThemeImpl();
        return;
    }

    const std::string sanitized = sanitizeThemeName(name);
    if (sanitized.empty())
    {
        Nui::WebApi::Console::warn("ThemeController::applyThemeNameImpl: invalid theme name rejected.");
        return;
    }

    // Skip reload if the same theme is already active
    if (activeCustomTheme_.has_value() && *activeCustomTheme_ == sanitized)
        return;

    unloadCustomThemeImpl();

    const std::string url = "nui://app.example/theme/" + sanitized + ".css";

    auto doc = Nui::val::global("document");
    auto head = doc["head"];
    if (head.isUndefined())
    {
        Nui::WebApi::Console::warn("ThemeController::applyThemeNameImpl: <head> not found.");
        return;
    }

    auto link = doc.call<Nui::val>("createElement", "link"s);
    link.set("rel", "stylesheet"s);
    link.set("id", "nui-custom-theme-link"s);
    link.set("href", url);
    head.call<void>("appendChild", link);

    doc["documentElement"]["dataset"].set("customTheme", sanitized);
    activeCustomTheme_ = sanitized;

    Nui::WebApi::Console::log("Loaded custom theme: " + sanitized + " from " + url);
}

// Public convenience — mirrors applyMode() in style
void ThemeController::applyThemeName(const std::string& name)
{
    events_->themeName = name;
    events_->themeName.eventContext().sync();
}

std::optional<std::string> ThemeController::getActiveCustomTheme() const
{
    return activeCustomTheme_;
}