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
     * @brief Why the upload of a watched path failed. NotQueued: the operation queue refused it.
     */
    BOOST_DEFINE_ENUM_CLASS(
        UploadFailureReason,
        LockedLocally,
        PermissionDeniedLocally,
        UnreadableLocally,
        PermissionDeniedOnServer,
        NoSpaceOnServer,
        ServerRefused,
        ConnectionProblem,
        NotQueued,
        Unknown
    )

    /**
     * @brief A path of a watch whose upload failed, it waits for Retry or Dismiss. Nothing retries it on its own.
     */
    struct UploadFailure
    {
        Ids::FailureId failureId;
        Ids::WatchId watchId;
        Paths::LocalPath localPath;
        Paths::RemotePath remotePath;
        UploadFailureReason reason{UploadFailureReason::Unknown};
        /** @brief The server's or the system's message, untranslated. */
        std::optional<std::string> detail{};
        /** @brief When it failed last, seconds since the Unix epoch. */
        std::uint64_t failedAt{0};
        /** @brief How often the upload of the path failed since it last went through. */
        std::uint64_t attempts{1};
    };
    BOOST_DESCRIBE_STRUCT(
        UploadFailure,
        (),
        (failureId, watchId, localPath, remotePath, reason, detail, failedAt, attempts)
    )
}
