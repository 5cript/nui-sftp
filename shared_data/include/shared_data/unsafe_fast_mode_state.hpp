#pragma once

#include <shared_data/shared_data.hpp>

namespace SharedData
{
    /**
     * @brief Whether the operation queue of a connection transfers small files in the unsafe fast mode.
     */
    struct UnsafeFastModeState
    {
        bool enabled;
    };
    BOOST_DESCRIBE_STRUCT(UnsafeFastModeState, (), (enabled))
}
