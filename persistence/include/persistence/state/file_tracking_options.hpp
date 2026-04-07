#pragma once

#include <persistence/state_core.hpp>

namespace Persistence
{
    struct FileTrackingOptions : public DefaultMissingMember
    {
        bool autoReupload{true};
        bool moveRemoteOnLocalMove{true};
        bool deleteRemoteOnLocalDelete{true};
    };

    BOOST_DESCRIBE_STRUCT(FileTrackingOptions, (), (autoReupload, moveRemoteOnLocalMove, deleteRemoteOnLocalDelete))
}
