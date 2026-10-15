#pragma once

#include <persistence/state_core.hpp>

#include <optional>

namespace Persistence
{
    struct FileTrackingOptions : public DefaultMissingMember
    {
        bool autoReupload{true};
        bool moveRemoteOnLocalMove{true};
        bool deleteRemoteOnLocalDelete{true};
        /** @brief Before a re-upload, ask when the server's copy changed since it was opened or last uploaded. Unset means true. */
        std::optional<bool> checkRemoteChanges{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(
        FileTrackingOptions,
        (),
        (autoReupload, moveRemoteOnLocalMove, deleteRemoteOnLocalDelete, checkRemoteChanges)
    )
}
