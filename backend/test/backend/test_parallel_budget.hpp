#pragma once

#include <backend/sftp/parallel_budget.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace Test
{
    namespace
    {
        class StubOperation : public Operation
        {
          public:
            StubOperation(int wantsSlots, bool barrier)
                : wantsSlots_{wantsSlots}
                , barrier_{barrier}
            {}

            SharedData::OperationType type() const override
            {
                return SharedData::OperationType::CustomAction;
            }

            RemoteIo::ProcessingStrand* strand() const override
            {
                return nullptr;
            }

            bool isBarrier() const noexcept override
            {
                return barrier_;
            }

            int parallelWorkDoable(int parallel) const noexcept override
            {
                lastOffer_ = parallel;
                return wantsSlots_;
            }

            std::expected<WorkStatus, Error> work() override
            {
                return WorkStatus::Complete;
            }

            std::expected<void, Error> cancel(bool) override
            {
                return {};
            }

            int lastOffer() const noexcept
            {
                return lastOffer_;
            }

          private:
            int wantsSlots_;
            bool barrier_;
            mutable int lastOffer_{-1};
        };

        using StubQueue = std::deque<std::pair<int, std::unique_ptr<StubOperation>>>;

        StubQueue makeQueue(std::initializer_list<std::pair<int, bool>> specs)
        {
            StubQueue queue;
            int index = 0;
            for (auto const& [wants, barrier] : specs)
                queue.emplace_back(index++, std::make_unique<StubOperation>(wants, barrier));
            return queue;
        }
    }

    TEST(ParallelBudgetTests, EmptyQueueYieldsNothing)
    {
        StubQueue queue;
        EXPECT_TRUE(collectEligibleOperations(queue, 4).empty());
    }

    TEST(ParallelBudgetTests, SingleSlotOperationsEachGetOneUntilBudgetIsSpent)
    {
        auto queue = makeQueue({{1, false}, {1, false}, {1, false}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 3);
        ASSERT_EQ(eligible.size(), 3u);
        for (std::size_t index = 0; index < eligible.size(); ++index)
        {
            EXPECT_EQ(eligible[index].index, index);
            EXPECT_EQ(eligible[index].slots, 1);
        }
    }

    TEST(ParallelBudgetTests, GreedyOperationLeavesRemainderForOthers)
    {
        // A bulk asking for 2 of 4 leaves 2 for the next two singles.
        auto queue = makeQueue({{2, false}, {1, false}, {1, false}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 4);
        ASSERT_EQ(eligible.size(), 3u);
        EXPECT_EQ(eligible[0].slots, 2);
        EXPECT_EQ(eligible[1].slots, 1);
        EXPECT_EQ(eligible[2].slots, 1);
        EXPECT_EQ(queue[0].second->lastOffer(), 4);
        EXPECT_EQ(queue[1].second->lastOffer(), 2);
        EXPECT_EQ(queue[2].second->lastOffer(), 1);
    }

    TEST(ParallelBudgetTests, RequestsAreClampedToTheRemainingBudget)
    {
        auto queue = makeQueue({{10, false}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 3);
        ASSERT_EQ(eligible.size(), 1u);
        EXPECT_EQ(eligible[0].slots, 3);
    }

    TEST(ParallelBudgetTests, EveryEligibleOperationGetsAtLeastOneSlot)
    {
        auto queue = makeQueue({{0, false}, {-5, false}});
        const auto eligible = collectEligibleOperations(queue, 2);
        ASSERT_EQ(eligible.size(), 2u);
        EXPECT_EQ(eligible[0].slots, 1);
        EXPECT_EQ(eligible[1].slots, 1);
    }

    TEST(ParallelBudgetTests, BarrierIsIncludedButStopsTheWalk)
    {
        auto queue = makeQueue({{1, false}, {1, true}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 4);
        ASSERT_EQ(eligible.size(), 2u);
        EXPECT_EQ(eligible[1].index, 1u);
    }

    TEST(ParallelBudgetTests, EligibleOperationsAreTheHeadOfTheQueueInOrder)
    {
        auto queue = makeQueue({{2, false}, {0, false}, {1, false}, {3, false}, {1, true}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 8);
        ASSERT_EQ(eligible.size(), 5u);
        for (std::size_t index = 0; index < eligible.size(); ++index)
            EXPECT_EQ(eligible[index].index, index);
    }

    TEST(ParallelBudgetTests, NonPositiveBudgetStillAdvancesTheHead)
    {
        auto queue = makeQueue({{1, false}, {1, false}});
        const auto eligible = collectEligibleOperations(queue, 0);
        ASSERT_EQ(eligible.size(), 1u);
        EXPECT_EQ(eligible[0].slots, 1);
    }

    TEST(ParallelBudgetTests, NoTransferLimitKeepsTheConfiguredParallelism)
    {
        const auto limits = parallelLimits(4, 3, std::nullopt);
        EXPECT_EQ(limits.parallelism, 4);
        EXPECT_EQ(limits.bulkConcurrency, 3);
    }

    TEST(ParallelBudgetTests, ATransferLimitLowersParallelismAndBulkConcurrency)
    {
        const auto limits = parallelLimits(4, 3, 2u);
        EXPECT_EQ(limits.parallelism, 2);
        EXPECT_EQ(limits.bulkConcurrency, 1);
    }

    TEST(ParallelBudgetTests, ATransferLimitAboveTheConfiguredParallelismChangesNothing)
    {
        const auto limits = parallelLimits(4, 3, 8u);
        EXPECT_EQ(limits.parallelism, 4);
        EXPECT_EQ(limits.bulkConcurrency, 3);
    }

    TEST(ParallelBudgetTests, ParallelismStaysAtLeastOne)
    {
        const auto limited = parallelLimits(4, 3, 0u);
        EXPECT_EQ(limited.parallelism, 1);
        EXPECT_EQ(limited.bulkConcurrency, 1);
        const auto unconfigured = parallelLimits(0, 0, std::nullopt);
        EXPECT_EQ(unconfigured.parallelism, 1);
        EXPECT_EQ(unconfigured.bulkConcurrency, 1);
    }

    TEST(ParallelBudgetTests, BulkConcurrencyLeavesASlotForOtherOperations)
    {
        EXPECT_EQ(parallelLimits(4, 8, std::nullopt).bulkConcurrency, 3);
        EXPECT_EQ(parallelLimits(8, 6, 4u).bulkConcurrency, 3);
    }

    TEST(ParallelBudgetTests, LimitingAgainKeepsTheLimits)
    {
        const auto first = parallelLimits(8, 6, 4u);
        const auto again = parallelLimits(first.parallelism, first.bulkConcurrency, 4u);
        EXPECT_EQ(again.parallelism, first.parallelism);
        EXPECT_EQ(again.bulkConcurrency, first.bulkConcurrency);
    }
}
