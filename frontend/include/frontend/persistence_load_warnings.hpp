#pragma once

#include <persistence/load_warning.hpp>

#include <string>

/**
 * @brief Translates config load warnings into the current language, one paragraph per warning.
 *
 * @param warnings The warnings reported by the backend state holder.
 * @return The translated text, empty if there are no warnings.
 */
std::string formatLoadWarnings(Persistence::LoadWarnings const& warnings);
