#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <shared_data/file_tracking/watch_options.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace SharedData
{
    /**
     * @brief What the file tracking panel shows of one watch of the session's tracker.
     */
    struct TrackedEntry
    {
        Ids::WatchId watchId;
        /** @brief The name of the watched local file or directory. */
        std::string name;
        /** @brief Where the watched item goes on the server. */
        Paths::RemotePath remotePath;
        WatchOptions options;
        /** @brief Uploads of the item are on their way. */
        bool uploading{false};
        /**
         * @brief The item is a temporary copy of the server's file (Open and Watch): its moves and its removal never
         *        reach the server, the options for those stay off.
         */
        bool downloadedCopy{false};
        /** @brief The watched local file or directory, for a downloaded copy its file in the instance directory. */
        Paths::LocalPath localPath{};
        bool isDirectory{false};
        /** @brief Set once the watch stopped, the entry stays until it is removed. */
        std::optional<WatchStop> stop{};
        /** @brief The instance directory of a downloaded copy. */
        std::optional<Ids::InstanceId> instanceId{};
        /** @brief The size of the server's file a downloaded copy was downloaded from, as it was then. */
        std::uint64_t sizeBytes{0};
    };
    BOOST_DESCRIBE_STRUCT(
        TrackedEntry,
        (),
        (watchId, name, remotePath, options, uploading, downloadedCopy, localPath, isDirectory, stop, instanceId, sizeBytes)
    )
}
