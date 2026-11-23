#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    /**
     * @brief What a cancel of the user's does with the partial files of a transfer. No option of the transfer decides
     *        it, doCleanup is for a download that fails.
     */
    BOOST_DEFINE_ENUM_CLASS(
        PartialFilesOnCancel,
        Deleted,
        Kept
    )
}
