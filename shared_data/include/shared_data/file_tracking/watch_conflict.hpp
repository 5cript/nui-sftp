#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <optional>

namespace SharedData
{
    /**
     * @brief What the server did to the copy a local change would replace, since it was opened or last uploaded.
     */
    BOOST_DEFINE_ENUM_CLASS(ConflictKind, ChangedOnServer, DeletedOnServer, CreatedOnServer)

    /**
     * @brief The user's answer to a WatchConflict. ReplaceOnServer uploads the local version, KeepServers leaves the
     *        server's and asks again on the next save.
     */
    BOOST_DEFINE_ENUM_CLASS(ConflictResolution, ReplaceOnServer, KeepServers)

    /**
     * @brief A local change of a watch that waits for the user, because the server's copy changed meanwhile.
     */
    struct WatchConflict
    {
        Ids::ConflictId conflictId;
        Ids::WatchId watchId;
        ConflictKind kind{ConflictKind::ChangedOnServer};
        Paths::LocalPath localPath;
        Paths::RemotePath remotePath;
        /** @brief The modification time of the server's copy, seconds since the Unix epoch. */
        std::optional<std::uint64_t> serverModifiedAt{};
        /** @brief The modification time of the local file, seconds since the Unix epoch. */
        std::optional<std::uint64_t> localModifiedAt{};
        /** @brief Seconds since the Unix epoch. */
        std::uint64_t raisedAt{0};
    };
    BOOST_DESCRIBE_STRUCT(
        WatchConflict,
        (),
        (conflictId, watchId, kind, localPath, remotePath, serverModifiedAt, localModifiedAt, raisedAt)
    )
}
