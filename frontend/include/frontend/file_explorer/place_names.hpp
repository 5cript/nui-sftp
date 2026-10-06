#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <string>

/**
 * @brief The icon for a default place, picked by its untranslated name such as "Home" or "Downloads".
 */
Nui::ElementRenderer iconForPlaceName(std::string const& name);

/**
 * @brief The translated label of a default place, or the name itself if it is not a known default place.
 */
std::string placeDisplayName(std::string const& name);
