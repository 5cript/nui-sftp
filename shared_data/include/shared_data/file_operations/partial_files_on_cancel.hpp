#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    /**
     * @brief What a cancel of the user's does with the partial files of a transfer. AsConfigured deletes them when the
     *        transfer's doCleanup option says so, Deleted and Kept decide it whatever the option says.
     */
    BOOST_DEFINE_ENUM_CLASS(
        PartialFilesOnCancel,
        AsConfigured,
        Deleted,
        Kept
    )
}
