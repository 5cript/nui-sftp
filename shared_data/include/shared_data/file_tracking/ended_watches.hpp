#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/file_tracking/tracked_entry.hpp>
#include <utility/describe.hpp>

#include <vector>

namespace SharedData
{
    /**
     * @brief Why watches ended that the user did not stop. CopyGone: the downloaded copy was deleted or renamed away
     *        and did not come back. ConnectionLost: the session lost its connection, either side may change until it
     *        is back. Overlap: the item moved onto the server path of another watch.
     */
    BOOST_DEFINE_ENUM_CLASS(WatchEndReason, CopyGone, ConnectionLost, Overlap)

    /**
     * @brief Watches of the session's tracker that ended for the same reason.
     */
    struct EndedWatches
    {
        WatchEndReason reason;
        /** @brief What the entries of the watches showed last. */
        std::vector<TrackedEntry> entries;
    };
    BOOST_DESCRIBE_STRUCT(EndedWatches, (), (reason, entries))
}
