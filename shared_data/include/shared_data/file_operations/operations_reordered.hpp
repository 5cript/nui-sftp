#pragma once

#include <ids/ids.hpp>
#include <utility/describe.hpp>

#include <cstdint>

namespace SharedData
{
    /**
     * @brief Backend → frontend notification fired after every `moveOperation`
     *        RPC dispatch, regardless of whether the move actually applied.
     *
     *        - `applied=true`: backend reordered its regular deque; frontend
     *          must mirror the change by moving the entry to @ref newIndex.
     *        - `applied=false`: backend refused (not paused, id is in the
     *          priority queue, op completed meanwhile, etc.); frontend
     *          should leave its view unchanged but is free to clear any
     *          pending UI affordance (drop indicator, etc.).
     *
     *        Sending the event on every branch lets the frontend treat this
     *        as a definitive resolution signal rather than a fire-and-pray.
     */
    struct OperationsReordered
    {
        Ids::OperationId operationId;
        // int32_t (not size_t/uint64_t) avoids a Nui RPC template-deduction
        // conflict in the uint64 hi/lo split path. Queue sizes never come
        // close to overflowing 2^31 in practice.
        std::int32_t newIndex{0};
        bool applied{false};
    };
    BOOST_DESCRIBE_STRUCT(OperationsReordered, (), (operationId, newIndex, applied))
}
