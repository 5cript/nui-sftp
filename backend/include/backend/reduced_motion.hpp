#pragma once

#include <persistence/state/ui_options.hpp>

#include <nlohmann/json.hpp>
#include <nui/window.hpp>

#include <string_view>

/// DevTools protocol method that overrides media features of the page.
inline constexpr std::string_view reducedMotionEmulationMethod = "Emulation.setEmulatedMedia";

/**
 * @brief Parameters of reducedMotionEmulationMethod that override prefers-reduced-motion. System removes the override.
 * @param preference The value the page sees.
 */
nlohmann::json reducedMotionEmulationParameters(Persistence::ReducedMotion preference);

/**
 * @brief Overrides the prefers-reduced-motion media feature of the window's page. Callable from any thread.
 * @details Chromium engines (CEF, WebView2) take it over the DevTools protocol, the others have no such override and
 *          leave it to the system.
 * @param window The main window.
 * @param preference The persisted setting.
 */
void applyReducedMotion(Nui::Window& window, Persistence::ReducedMotion preference);
