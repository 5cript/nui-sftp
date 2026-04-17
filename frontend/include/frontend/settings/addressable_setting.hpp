#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <string>

/**
 * @brief Wraps a setting (or any settings-panel element) in a div with a DOM
 *        id so that external code can jump to it via
 *        `FrontendEvents::requestOpenSettingsAtId(htmlId)`.
 *
 * Use at the call site in a settings section's render tree:
 *
 * @code
 *     addressableSetting("general-showLocalShellWarning",
 *         userInterface.showLocalShellWarning(language->getObserved(...))
 *     )
 * @endcode
 *
 * The helper only adds the id + marker class; it is otherwise transparent, so
 * styling and layout behave as if the inner renderer were used directly.
 * A small highlight animation is applied when the Settings component resolves
 * a scroll request — see `.addressable-setting--highlight` in settings.css.
 *
 * The Settings component walks up from this element to the nearest
 * `[data-settings-section]` ancestor to decide which section to activate
 * before scrolling.
 */
Nui::ElementRenderer addressableSetting(std::string htmlId, Nui::ElementRenderer inner);