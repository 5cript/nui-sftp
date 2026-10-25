#pragma once

#include <backend/sftp/operation.hpp>

#include <chrono>
#include <expected>
#include <string>

namespace Benchmark
{
    /**
     * @brief Works @p operation until it completes, the way OperationQueue::workQueue works the head of its queue:
     *        with the slot budget of the default concurrency and inside the SFTP strand when it uses it. Like
     *        Session::doOperationQueueWork it works again right away after progress and sleeps until the operation
     *        wakes it while it waits.
     */
    std::expected<void, std::string> runToCompletion(Operation& operation, std::chrono::seconds timeout);

    /** @brief The queue's default slot budget, see Persistence::defaultSftpConcurrency. */
    int queueParallelism();

    /** @brief What OperationQueue gives a bulk transfer by default, see Persistence::defaultBulkConcurrency. */
    int bulkConcurrency();
}
