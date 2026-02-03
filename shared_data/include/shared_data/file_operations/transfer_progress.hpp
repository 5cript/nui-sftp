#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace SharedData
{
    struct TransferProgress
    {
        Ids::OperationId operationId;
        std::uint64_t min;
        std::uint64_t max;
        std::uint64_t current;
        std::make_signed_t<std::size_t> bytesPerSecond;
    };
    BOOST_DESCRIBE_STRUCT(TransferProgress, (), (operationId, min, max, current, bytesPerSecond))
}