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
        // Pinned to int64 on the wire so the describe-based JSON layer uses
        // the `{_u64_hi, _u64_lo}` split encoding symmetrically on both
        // sides.  `std::make_signed_t<std::size_t>` is 64-bit on the
        // backend but 32-bit `long` on the WASM frontend — that mismatch
        // routes the value through the fundamental `val.as<long>()` path
        // on an object and throws "Cannot convert '[object Object]' to long".
        std::int64_t bytesPerSecond;
    };
    BOOST_DESCRIBE_STRUCT(TransferProgress, (), (operationId, min, max, current, bytesPerSecond))
}