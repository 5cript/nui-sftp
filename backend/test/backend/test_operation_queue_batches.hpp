#pragma once

#include "test_operation_queue.hpp"
#include <backend/sftp/operation_queue.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace Test
{
    /**
     * @brief The queue with operations on a file system in memory, whose removals can be held on its processing thread.
     */
    class OperationQueueBatchTests : public OperationQueueTests
    {
      protected:
        OperationQueueBatchTests()
        {
            fakeThread_.start(std::chrono::milliseconds{1});
        }
        ~OperationQueueBatchTests() override
        {
            fakeThread_.stop();
        }
        OperationQueueBatchTests(OperationQueueBatchTests const&) = delete;
        OperationQueueBatchTests& operator=(OperationQueueBatchTests const&) = delete;
        OperationQueueBatchTests(OperationQueueBatchTests&&) = delete;
        OperationQueueBatchTests& operator=(OperationQueueBatchTests&&) = delete;

        static constexpr auto waitLimit = std::chrono::seconds{5};

        /**
         * @brief An open held on the processing thread until released.
         */
        struct HeldOpen
        {
            std::promise<void> entering{};
            std::shared_future<void> hasEntered{entering.get_future().share()};
            std::promise<void> releasing{};
            std::shared_future<void> released{releasing.get_future().share()};
        };

        /**
         * @brief Holds the first open on the fake until the test releases it.
         */
        std::shared_ptr<HeldOpen> holdFirstOpen()
        {
            auto held = std::make_shared<HeldOpen>();
            fake_->failWhen<RemoteIo::OpenFile>(
                [held, first = std::make_shared<std::atomic_bool>(true)](RemoteIo::OpenFile const&)
                {
                    if (first->exchange(false))
                    {
                        held->entering.set_value();
                        held->released.wait();
                    }
                    return false;
                },
                {}
            );
            return held;
        }

        /**
         * @brief Runs @p function on the queue's strand without waiting for it.
         * @return Ready once it ran.
         */
        std::future<void> postToStrand(std::function<void()> function = {})
        {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            boost::asio::post(
                *strand_,
                [promise, function = std::move(function)]()
                {
                    if (function)
                        function();
                    promise->set_value();
                }
            );
            return future;
        }

        /**
         * @brief Waits until the batch on the fake's processing thread returned to the queue's strand.
         */
        void waitForTheBatchToReturn()
        {
            fakeStrand_->pushPromiseTask([]() {}).wait();
            runOnStrand([]() {});
        }

        /**
         * @brief One cycle of the queue, its batch included.
         */
        bool workCycle(OperationQueue& queue)
        {
            return runOnStrand(
                [&queue]()
                {
                    return queue.work() || queue.drainBatch();
                }
            );
        }

        /**
         * @brief Queues the download of the fake's file @p name into the local directory. The file takes several reads,
         *        so the download runs for several cycles.
         */
        Ids::OperationId addDownload(OperationQueue& queue, std::string const& name)
        {
            const auto remotePath = remote(fmt::format("/home/test/{}", name));
            fake_->addFile(remotePath, std::string(8u * 1024u, 'x'));
            auto operationId = Ids::generateOperationId();
            const auto added = runOnStrand(
                [&]()
                {
                    return queue.addDownloadOperation(
                        fake_,
                        operationId,
                        toLocal(localDirectory_.path() / name),
                        remotePath,
                        false,
                        false,
                        false,
                        false
                    );
                }
            );
            EXPECT_TRUE(added.has_value()) << added.error().toString();
            return operationId;
        }

        std::vector<std::string> completedIds()
        {
            std::vector<std::string> ids;
            for (auto const& completed : completions())
                ids.push_back(completed.operationId.value());
            return ids;
        }

        RemoteIo::ProcessingThread fakeThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> fakeStrand_{fakeThread_.createStrand()};
        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> fake_{
            std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                fakeStrand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{}
            )
        };
    };

    TEST_F(OperationQueueBatchTests, TheStrandStaysFreeWhileABatchWorksOnTheProcessingThread)
    {
        const auto held = holdFirstOpen();
        auto queue = makeQueue();
        setPaused(false);
        const auto operationId = addDownload(*queue, "file.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        EXPECT_EQ(postToStrand().wait_for(waitLimit), std::future_status::ready) << "the strand waited for the batch";
        held->releasing.set_value();

        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().operationId, operationId);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / "file.txt"));
    }

    TEST_F(OperationQueueBatchTests, ACancelWhileTheBatchIsOutEndsTheOperationOnceTheBatchReturned)
    {
        const auto held = holdFirstOpen();
        auto queue = makeQueue();
        setPaused(false);
        const auto operationId = addDownload(*queue, "file.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        queue->cancel(operationId);
        held->releasing.set_value();

        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_FALSE(std::filesystem::exists(localDirectory_.path() / "file.txt"));
    }

    TEST_F(OperationQueueBatchTests, AMoveWhileThePausedQueueHasABatchOutAppliesOnceTheBatchReturned)
    {
        const auto held = holdFirstOpen();
        auto queue = makeQueue();
        runOnStrand(
            [&queue]()
            {
                queue->limitParallelism(1u);
            }
        );
        setPaused(false);
        const auto first = addDownload(*queue, "a.txt");
        const auto second = addDownload(*queue, "b.txt");
        const auto third = addDownload(*queue, "c.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        setPaused(true);
        queue->moveOperation(third, 0);
        held->releasing.set_value();
        waitForTheBatchToReturn();

        setPaused(false);
        workUntilCompletions(3);
        EXPECT_EQ(completedIds(), (std::vector<std::string>{third.value(), first.value(), second.value()}));
    }

    TEST_F(OperationQueueBatchTests, AQueueGoneWhileItsBatchIsOutLeavesItsOperationsToTheBatch)
    {
        const auto held = holdFirstOpen();
        auto queue = makeQueue();
        setPaused(false);
        addDownload(*queue, "file.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        runOnStrand(
            [&]()
            {
                queue.reset();
                queue_.reset();
            }
        );
        held->releasing.set_value();
        waitForTheBatchToReturn();
        fakeStrand_->pushPromiseTask([]() {}).wait();

        EXPECT_TRUE(completions().empty());
        EXPECT_EQ(fake_->openStreams(), 0u) << "the download ended with its batch";
    }

    TEST_F(OperationQueueBatchTests, OperationsThatEndInOneBatchCompleteOnePerCycle)
    {
        auto queue = makeQueue();
        setPaused(false);
        addDownload(*queue, "a.txt");
        addDownload(*queue, "b.txt");

        for (int cycle = 0; cycle < 50 && completions().empty(); ++cycle)
            workCycle(*queue);
        runOnStrand([]() {});
        EXPECT_EQ(completions().size(), 1u);

        for (int cycle = 0; cycle < 50 && completions().size() < 2u; ++cycle)
            workCycle(*queue);
        runOnStrand([]() {});
        ASSERT_EQ(completions().size(), 2u);
        for (auto const& completed : completions())
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed);
    }
}
