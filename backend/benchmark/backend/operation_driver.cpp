#include "operation_driver.hpp"

#include <backend/sftp/parallel_budget.hpp>
#include <persistence/state/sftp_options.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Benchmark
{
    namespace
    {
        std::expected<Operation::WorkStatus, Operation::Error> workOnce(Operation& operation)
        {
            const std::vector<std::pair<int, Operation*>> queue{{0, &operation}};
            const auto eligible = collectEligibleOperations(queue, queueParallelism());
            operation.setParallelBudget(eligible.front().slots);

            if (!operation.usesStrand())
                return operation.work();
            auto* const strand = operation.strand();
            if (strand == nullptr)
                return operation.workInStrand();
            return strand
                ->pushPromiseTask(
                    [&operation]()
                    {
                        return operation.workInStrand();
                    }
                )
                .get();
        }
    }

    int queueParallelism()
    {
        return Persistence::defaultSftpConcurrency;
    }

    int bulkConcurrency()
    {
        return std::clamp(Persistence::defaultBulkConcurrency, 1, std::max(1, queueParallelism() - 1));
    }

    std::expected<void, std::string> runToCompletion(Operation& operation, std::chrono::seconds timeout)
    {
        // Shared with the wake: a transfer that outlives a failed run may still call it.
        struct WakeState
        {
            std::mutex mutex{};
            std::condition_variable woken{};
            bool requested{false};
        };
        const auto state = std::make_shared<WakeState>();
        operation.setWake(
            [state]()
            {
                {
                    const std::scoped_lock lock{state->mutex};
                    state->requested = true;
                }
                state->woken.notify_one();
            }
        );

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                const std::scoped_lock lock{state->mutex};
                state->requested = false;
            }
            const auto result = workOnce(operation);
            if (!result)
                return std::unexpected{result.error().toString()};
            if (*result == Operation::WorkStatus::Complete)
                return {};
            if (*result == Operation::WorkStatus::Waiting)
            {
                std::unique_lock lock{state->mutex};
                state->woken.wait_until(
                    lock,
                    deadline,
                    [&state]
                    {
                        return state->requested;
                    }
                );
            }
        }
        return std::unexpected{fmt::format("did not complete within {} s", timeout.count())};
    }
}
