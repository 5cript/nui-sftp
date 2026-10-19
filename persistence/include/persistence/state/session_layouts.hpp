#pragma once

#include <persistence/state_core.hpp>
#include <persistence/state/session_options.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace Persistence
{
    /**
     * @brief The saved panel layouts, shared by all sessions of a type.
     */
    struct SessionLayouts : public DefaultMissingMember
    {
        std::map<std::string, nlohmann::json> ssh{};
        std::map<std::string, nlohmann::json> shell{};

        /**
         * @brief The layouts shared by the sessions of a type.
         *
         * @param type The type of the sessions.
         */
        std::map<std::string, nlohmann::json>& of(TerminalEngineType type);

        /**
         * @brief The layouts shared by the sessions of a type.
         *
         * @param type The type of the sessions.
         */
        std::map<std::string, nlohmann::json> const& of(TerminalEngineType type) const;
    };
    BOOST_DESCRIBE_STRUCT(SessionLayouts, (), (ssh, shell))
}
