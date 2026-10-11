#pragma once

#include <nlohmann/json.hpp>

namespace Persistence
{
    /**
     * @brief Rewrites entries of earlier versions into their current form before the config is read. Copies the
     * single user and key files of SSH servers into their user list, keeping them for earlier versions.
     *
     * @param config The config as read from disk.
     * @return Whether anything was rewritten.
     */
    bool migrateLegacyEntries(nlohmann::json& config);
}
