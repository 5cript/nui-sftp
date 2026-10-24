#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace SharedData
{
    /**
     * @brief A file of a watch on its way to the server.
     */
    struct WatchFileProgress
    {
        Paths::LocalPath path;
        std::uint64_t bytes{0};
        std::uint64_t totalBytes{0};
    };
    BOOST_DESCRIBE_STRUCT(WatchFileProgress, (), (path, bytes, totalBytes))

    /**
     * @brief The last burst of changes of a watch that ended.
     */
    struct WatchSync
    {
        /** @brief Seconds since the Unix epoch. */
        std::uint64_t endedAt{0};
        std::uint64_t changes{0};
        std::uint64_t bytes{0};
    };
    BOOST_DESCRIBE_STRUCT(WatchSync, (), (endedAt, changes, bytes))

    /**
     * @brief How far one watch is with mirroring its changes to the server.
     */
    struct WatchProgress
    {
        Ids::WatchId watchId;
        /** @brief Changes on their way: waiting for the check, an answer or the queue, or running. */
        std::uint64_t changesPending{0};
        /** @brief Changes of the current burst that ended. */
        std::uint64_t changesDone{0};
        /** @brief Pending changes that have not started on the server yet. */
        std::uint64_t changesWaiting{0};
        /** @brief Pending changes that wait for the user's answer to a conflict. */
        std::uint64_t waitingForAnswer{0};
        std::uint64_t bytesDone{0};
        std::uint64_t bytesTotal{0};
        std::vector<WatchFileProgress> inFlight{};
        std::optional<WatchSync> lastSync{};
    };
    BOOST_DESCRIBE_STRUCT(
        WatchProgress,
        (),
        (watchId, changesPending, changesDone, changesWaiting, waitingForAnswer, bytesDone, bytesTotal, inFlight, lastSync)
    )
}
