#pragma once

#include <persistence/state_core.hpp>

#include <optional>

namespace Persistence
{
    struct QueueOptions : public DefaultMissingMember
    {
        std::optional<bool> autoRemoveCompletedOperations{std::nullopt};
        std::optional<bool> startInPausedState{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(QueueOptions, (), (autoRemoveCompletedOperations, startInPausedState))
}