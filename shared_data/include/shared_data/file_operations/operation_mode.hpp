#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(
        OperationMode,
        Queued,
        PriorityQueued
    )
}
