#pragma once

#include <backend/sftp/operation.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace Benchmark
{
    enum class Pacing
    {
        /** @brief Like Session::doOperationQueueWork: a pause after every ten back to back work() calls. */
        Session,
        /** @brief work() again right away, what the operation could do without the queue's throttle. */
        Unthrottled,
    };

    std::string_view toString(Pacing pacing);

    /**
     * @brief Works @p operation until it completes, the way OperationQueue::workQueue works the head of its queue:
     *        with the slot budget of the default concurrency and inside the SFTP strand when it uses it.
     */
    std::expected<void, std::string>
    runToCompletion(Operation& operation, Pacing pacing, std::chrono::seconds timeout);

    /** @brief The queue's default slot budget, see Persistence::defaultSftpConcurrency. */
    int queueParallelism();

    /** @brief What OperationQueue gives a bulk transfer by default, see Persistence::defaultBulkConcurrency. */
    int bulkConcurrency();
}
