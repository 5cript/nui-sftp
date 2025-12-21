#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace SharedData
{
    struct UploadProgress
    {
        Ids::OperationId operationId;
        std::uint64_t min;
        std::uint64_t max;
        std::uint64_t current;
    };
    BOOST_DESCRIBE_STRUCT(UploadProgress, (), (operationId, min, max, current))
}