#pragma once

#include <backend/sftp/operation.hpp>

#include <chrono>
#include <expected>
#include <thread>

namespace Test
{
    /**
     * @brief Works @p operation until it completes or fails, at most for @p limit. Waiting work returns at once, so it
     *        is polled with a short pause, as the queue drives it.
     *
     * @return What the last turn of work returned, FutureTimeout when @p limit ran out.
     */
    template <typename OperationT>
    std::expected<Operation::WorkStatus, Operation::Error>
    runToEnd(OperationT& operation, std::chrono::seconds limit = std::chrono::seconds{20})
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto result = operation.work();
            if (!result.has_value() || result.value() == Operation::WorkStatus::Complete)
                return result;
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
    }

    /**
     * @brief Works @p operation until @p reached tells so, at most for @p limit, polled like runToEnd().
     *
     * @return Whether it got there before the operation ended.
     */
    template <typename OperationT, typename ConditionT>
    bool
    workUntil(OperationT& operation, ConditionT const& reached, std::chrono::seconds limit = std::chrono::seconds{20})
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!reached() && std::chrono::steady_clock::now() < deadline)
        {
            const auto result = operation.work();
            if (!result.has_value() || result.value() == Operation::WorkStatus::Complete)
                return false;
            if (result.value() == Operation::WorkStatus::Waiting)
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return reached();
    }
}
