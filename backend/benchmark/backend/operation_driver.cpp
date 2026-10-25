#include "operation_driver.hpp"

#include <backend/session.hpp>
#include <backend/sftp/parallel_budget.hpp>
#include <persistence/state/sftp_options.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <thread>
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

    std::string_view toString(Pacing pacing)
    {
        switch (pacing)
        {
            case Pacing::Session:
                return "queue pacing";
            case Pacing::Unthrottled:
                return "no pacing";
        }
        return "unknown";
    }

    int queueParallelism()
    {
        return Persistence::defaultSftpConcurrency;
    }

    int bulkConcurrency()
    {
        return std::clamp(Persistence::defaultBulkConcurrency, 1, std::max(1, queueParallelism() - 1));
    }

    std::expected<void, std::string> runToCompletion(Operation& operation, Pacing pacing, std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int unthrottled = 0;
        std::chrono::milliseconds throttle = Session::queueStartThrottle;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto result = workOnce(operation);
            if (!result)
                return std::unexpected{result.error().toString()};
            if (*result == Operation::WorkStatus::Complete)
                return {};
            if (pacing == Pacing::Unthrottled)
                continue;

            if (++unthrottled < 10)
            {
                throttle = Session::queueStartThrottle;
                continue;
            }
            unthrottled = 0;
            throttle = std::min<std::chrono::milliseconds>(throttle * 2, Session::queueMaxThrottle);
            std::this_thread::sleep_for(throttle);
        }
        return std::unexpected{fmt::format("did not complete within {} s", timeout.count())};
    }
}
