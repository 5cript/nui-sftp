#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace SharedData
{
    /**
     * @brief Why a watch stopped. StoppedByUser: the user stopped it. Overlap: its item moved onto the server path of
     *        another watch. CopyGone: the downloaded copy was deleted or renamed away and did not come back.
     *        ConnectionLost: the session lost its connection.
     */
    BOOST_DEFINE_ENUM_CLASS(WatchStopReason, StoppedByUser, Overlap, CopyGone, ConnectionLost)

    /**
     * @brief How a watch stopped, its entry stays listed until it is removed.
     */
    struct WatchStop
    {
        WatchStopReason reason{WatchStopReason::StoppedByUser};
        /** @brief Seconds since the Unix epoch. */
        std::uint64_t stoppedAt{0};
        /** @brief Overlap: the watch whose server path the item moved onto. */
        std::optional<Ids::WatchId> otherWatchId{};
        /** @brief Overlap: the name of that watch's item. */
        std::optional<std::string> otherWatchName{};
        /** @brief Overlap: where the item moved to locally. */
        std::optional<Paths::LocalPath> movedTo{};
        /** @brief Overlap: where its server copy would have moved to. */
        std::optional<Paths::RemotePath> wouldGoTo{};
    };
    BOOST_DESCRIBE_STRUCT(WatchStop, (), (reason, stoppedAt, otherWatchId, otherWatchName, movedTo, wouldGoTo))
}
