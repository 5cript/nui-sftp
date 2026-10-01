#pragma once

#include <backend/sftp/operation.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

/**
 * @brief One queue entry that may advance this tick, with the transfer slots granted to it.
 */
struct EligibleOperation
{
    std::size_t index;
    int slots;
};

/**
 * @brief Walks the head of a queue and distributes @p budget transfer slots over it.
 *
 * Every operation is asked how many slots it could use via Operation::parallelWorkDoable and
 * gets at least one. The walk stops when the budget is spent or right after a barrier operation.
 *
 * @param queue Anything indexable whose elements expose `.second` as a pointer-like Operation.
 * @param budget Slots available this tick; treated as at least 1.
 */
template <typename QueueT>
std::vector<EligibleOperation> collectEligibleOperations(QueueT const& queue, int budget)
{
    std::vector<EligibleOperation> eligible;
    int remaining = std::max(1, budget);
    for (std::size_t index = 0; index < queue.size() && remaining > 0; ++index)
    {
        auto const& operation = *queue[index].second;
        const int slots = std::clamp(operation.parallelWorkDoable(remaining), 1, remaining);
        eligible.push_back({.index = index, .slots = slots});
        remaining -= slots;
        if (operation.isBarrier())
            break;
    }
    return eligible;
}
