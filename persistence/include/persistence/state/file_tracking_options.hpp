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
        /// Before a re-upload, ask when the server's copy changed since it was opened. Unset means true, optional so
        /// configurations written before it load without a warning.
        std::optional<bool> checkRemoteChanges{std::nullopt};
    };

    BOOST_DESCRIBE_STRUCT(
        FileTrackingOptions,
        (),
        (autoReupload, moveRemoteOnLocalMove, deleteRemoteOnLocalDelete, checkRemoteChanges)
    )
}
