#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <cstdint>

namespace SharedData
{
    /**
     * @brief How far the tracker of a session is with mirroring the changes of its watches, apart from the operation
     *        queue: the panel shows it while something is on its way.
     */
    struct TrackerProgress
    {
        /** @brief Changes on their way to the server: waiting for the check, an answer or the queue, or running. */
        std::uint64_t pending{0};
        /** @brief Changes that ended since nothing was on its way any more. */
        std::uint64_t done{0};
        /** @brief Changes among the pending ones that wait for the user's answer to whether the server's copy goes. */
        std::uint64_t waitingForAnswer{0};

        bool operator==(TrackerProgress const&) const = default;
    };
    BOOST_DESCRIBE_STRUCT(TrackerProgress, (), (pending, done, waitingForAnswer))
}
