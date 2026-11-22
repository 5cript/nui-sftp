#pragma once

#include <backend/sftp/operation.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
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
 * @return The head of @p queue in order, entry i for queue[i], none skipped: the queue works its first that many.
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

/**
 * @brief The transfer slots of a queue and the most of them a single bulk may take.
 */
struct ParallelLimits
{
    int parallelism;
    int bulkConcurrency;
};

/**
 * @brief Lowers @p parallelism to @p maxParallelTransfers when that is set, never below one, and keeps
 *        @p bulkConcurrency below the result so another operation keeps a slot while there are two.
 */
inline ParallelLimits
parallelLimits(int parallelism, int bulkConcurrency, std::optional<std::size_t> maxParallelTransfers)
{
    auto limited = std::max(1, parallelism);
    if (maxParallelTransfers && *maxParallelTransfers < static_cast<std::size_t>(limited))
        limited = std::max(1, static_cast<int>(*maxParallelTransfers));
    return {.parallelism = limited, .bulkConcurrency = std::clamp(bulkConcurrency, 1, std::max(1, limited - 1))};
}
