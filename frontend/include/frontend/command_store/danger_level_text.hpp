#pragma once

#include <command-store/types.hpp>
#include <utility/language.hpp>

#include <optional>
#include <string>

/**
 * @brief The translated name of a snippet danger level, "Not rated" when unset.
 */
inline std::string dangerLevelText(std::optional<CommandStore::DangerLevel> level)
{
    if (!level)
        return language->get("snippetDanger", "unrated");
    switch (*level)
    {
        case CommandStore::DangerLevel::Safe:
            return language->get("snippetDanger", "safe");
        case CommandStore::DangerLevel::Caution:
            return language->get("snippetDanger", "caution");
        case CommandStore::DangerLevel::Danger:
            return language->get("snippetDanger", "danger");
    }
    return {};
}
