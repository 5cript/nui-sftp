#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>

#include <vector>

namespace SharedData
{
    /**
     * @brief Conflicts that left the list together: answered, or dropped with their watch.
     */
    struct RemovedConflicts
    {
        std::vector<Ids::ConflictId> conflictIds{};
    };
    BOOST_DESCRIBE_STRUCT(RemovedConflicts, (), (conflictIds))

    /**
     * @brief Failed uploads that left the list together: retried, dismissed, uploaded since, or dropped with their
     *        watch.
     */
    struct RemovedFailures
    {
        std::vector<Ids::FailureId> failureIds{};
    };
    BOOST_DESCRIBE_STRUCT(RemovedFailures, (), (failureIds))
}
