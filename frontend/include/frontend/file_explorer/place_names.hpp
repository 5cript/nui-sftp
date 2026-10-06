#pragma once

#include <nui/frontend/element_renderer.hpp>

#include <string>

/**
 * @brief The icon for a default place, picked by its kind such as "home" or "downloads".
 */
Nui::ElementRenderer iconForPlaceKind(std::string const& kind);

/**
 * @brief The translated label of a default place.
 *
 * @param kind The kind of the place, such as "home" or "downloads".
 * @param fallback Shown for kinds without a translation.
 */
std::string placeDisplayName(std::string const& kind, std::string const& fallback);
