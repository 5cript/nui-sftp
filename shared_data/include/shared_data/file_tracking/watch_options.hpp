#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(
        WatchOption,
        AutoReupload,
        MoveRemoteOnLocalMove,
        DeleteRemoteOnLocalDelete,
        CheckRemoteChanges
    )

    /**
     * @brief What one watch mirrors to the server, it starts with Persistence::FileTrackingOptions.
     */
    struct WatchOptions
    {
        bool autoReupload{true};
        bool moveRemoteOnLocalMove{true};
        bool deleteRemoteOnLocalDelete{true};
        /** @brief Ask before replacing a server copy that changed since it was opened or last uploaded. */
        bool checkRemoteChanges{true};

        bool& operator[](WatchOption option)
        {
            switch (option)
            {
                case WatchOption::AutoReupload:
                    return autoReupload;
                case WatchOption::MoveRemoteOnLocalMove:
                    return moveRemoteOnLocalMove;
                case WatchOption::DeleteRemoteOnLocalDelete:
                    return deleteRemoteOnLocalDelete;
                case WatchOption::CheckRemoteChanges:
                    break;
            }
            return checkRemoteChanges;
        }

        friend bool operator==(WatchOptions const&, WatchOptions const&) = default;
    };
    BOOST_DESCRIBE_STRUCT(
        WatchOptions,
        (),
        (autoReupload, moveRemoteOnLocalMove, deleteRemoteOnLocalDelete, checkRemoteChanges)
    )
}
