#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    /**
     * @brief Why a local file could not be read or written. Locked: another program holds it.
     */
    BOOST_DEFINE_ENUM_CLASS(LocalFileProblem, Locked, PermissionDenied, NotFound, NoSpace, Other)
}
