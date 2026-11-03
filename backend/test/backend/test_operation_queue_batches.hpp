#pragma once

#include "test_operation_queue.hpp"
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

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

        void TearDown() override
        {
            // A test that failed while it held an open, the queue's teardown waits for that batch.
            releaseHeldOpens();
            OperationQueueTests::TearDown();
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
            std::atomic_bool released{false};

            void release()
            {
                released = true;
                released.notify_all();
            }
        };

        /**
         * @brief Holds the first open on the fake that @p matches until the test releases it, or until the test ends.
         */
        std::shared_ptr<HeldOpen> holdOpen(std::function<bool(RemoteIo::OpenFile const&)> matches)
        {
            auto held = std::make_shared<HeldOpen>();
            heldOpens_.push_back(held);
            fake_->failWhen<RemoteIo::OpenFile>(
                [held, matches = std::move(matches), first = std::make_shared<std::atomic_bool>(true)](
                    RemoteIo::OpenFile const& open
                )
                {
                    if (matches(open) && first->exchange(false))
                    {
                        held->entering.set_value();
                        held->released.wait(false);
                    }
                    return false;
                },
                {}
            );
            return held;
        }

        std::shared_ptr<HeldOpen> holdFirstOpen()
        {
            return holdOpen(
                [](RemoteIo::OpenFile const&)
                {
                    return true;
                }
            );
        }

        void releaseHeldOpens()
        {
            for (auto const& held : heldOpens_)
                held->release();
        }

        /**
         * @brief Runs @p function on the queue's strand and waits for it while an open is held. A strand that waits
         *        for the held batch fails the test and gets the opens released, so it ends.
         */
        template <typename FunctionT>
        auto runOnStrandWhileHeld(FunctionT&& function) -> std::invoke_result_t<FunctionT>
        {
            auto future = postToStrand(std::forward<FunctionT>(function));
            if (future.wait_for(waitLimit) != std::future_status::ready)
            {
                ADD_FAILURE() << "the strand waited for the batch";
                releaseHeldOpens();
            }
            return future.get();
        }

        /**
         * @brief Waits until the batch on the fake's processing thread returned to the queue's strand.
         */
        void waitForTheBatchToReturn()
        {
            EXPECT_EQ(fakeStrand_->pushPromiseTask([]() {}).wait_for(waitLimit), std::future_status::ready);
            runOnStrandWhileHeld([]() {});
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
         * @brief Puts the file @p name on the fake. It takes several reads, so its download runs for several cycles.
         *        The fake stays locked while it holds an open, so files go in before.
         */
        Paths::RemotePath addRemoteFile(std::string const& name)
        {
            const auto remotePath = remote(fmt::format("/home/test/{}", name));
            fake_->addFile(remotePath, std::string(8u * 1024u, 'x'));
            return remotePath;
        }

        /**
         * @brief Queues the download of the fake's file @p name, see addRemoteFile(), into the local directory.
         */
        Ids::OperationId queueDownload(OperationQueue& queue, std::string const& name)
        {
            auto operationId = Ids::generateOperationId();
            const auto added = runOnStrandWhileHeld(
                [&]()
                {
                    return queue.addDownloadOperation(
                        fake_,
                        operationId,
                        toLocal(localDirectory_.path() / name),
                        remote(fmt::format("/home/test/{}", name)),
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

        /**
         * @brief Puts the file @p name on the fake and queues its download.
         */
        Ids::OperationId addDownload(OperationQueue& queue, std::string const& name)
        {
            addRemoteFile(name);
            return queueDownload(queue, name);
        }

        /**
         * @brief Queues the download of the fake's files @p names as one bulk, which asks the server nothing until it
         *        runs, so it is queued while an open is held too.
         * @return The id of its card.
         */
        Ids::OperationId queueBulkDownload(OperationQueue& queue, std::vector<std::string> const& names)
        {
            SharedData::BulkDownloadRequest request{};
            for (auto const& name : names)
            {
                request.entries.push_back({
                    .source = remote(fmt::format("/home/test/{}", name)),
                    .destination = toLocal(localDirectory_.path() / name),
                    .sizeBytes = 8u * 1024u,
                });
            }
            auto card = Ids::generateOperationId();
            const auto queued = runOnStrandWhileHeld(
                [&]()
                {
                    return queue.addBulkDownloadOperation(
                        fake_,
                        std::move(request),
                        [](std::size_t)
                        {
                            return Ids::generateOperationId();
                        },
                        card
                    );
                }
            );
            EXPECT_EQ(queued, names.size());
            return card;
        }

        /**
         * @brief Works @p queue until @p held holds its open.
         */
        void workUntilHeld(OperationQueue& queue, HeldOpen const& held)
        {
            const auto deadline = std::chrono::steady_clock::now() + 20 * waitLimit;
            while (held.hasEntered.wait_for(std::chrono::milliseconds{2}) != std::future_status::ready)
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return ADD_FAILURE() << "the open was never held";
                runOnStrandWhileHeld(
                    [&queue]()
                    {
                        queue.work();
                    }
                );
            }
        }

        std::vector<std::string> completedIds()
        {
            std::vector<std::string> ids;
            for (auto const& completed : completions())
                ids.push_back(completed.operationId.value());
            return ids;
        }

        std::vector<std::shared_ptr<HeldOpen>> heldOpens_{};
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
        runOnStrandWhileHeld([]() {});
        held->release();

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
        held->release();

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
        queue->paused(true);
        queue->moveOperation(third, 0);
        held->release();
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
        runOnStrandWhileHeld(
            [&]()
            {
                queue.reset();
                queue_.reset();
            }
        );
        held->release();
        waitForTheBatchToReturn();
        EXPECT_EQ(fakeStrand_->pushPromiseTask([]() {}).wait_for(waitLimit), std::future_status::ready);

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

    TEST_F(OperationQueueBatchTests, ARequestThatWaitsForTheBatchFirstOutlastsABatchLongerThanItsTimeout)
    {
        constexpr auto operationTimeout = std::chrono::seconds{1};
        const auto held = holdFirstOpen();
        Persistence::SftpOptions options{};
        options.operationTimeout = operationTimeout;
        auto queue = makeQueue(std::move(options));
        setPaused(false);
        addDownload(*queue, "held.txt");
        const auto remotePath = addRemoteFile("later.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        auto added = postToStrand(
            [&]()
            {
                // As the session does before a request that waits for the processing thread.
                queue->waitForBatch();
                return queue->addDownloadOperation(
                    fake_,
                    Ids::generateOperationId(),
                    toLocal(localDirectory_.path() / "later.txt"),
                    remotePath,
                    false,
                    false,
                    false,
                    false
                );
            }
        );
        // Held past the add's timeout: an add that counted the batch's time gave up meanwhile.
        EXPECT_EQ(added.wait_for(2 * operationTimeout), std::future_status::timeout) << "the add ended while held";
        held->release();

        ASSERT_EQ(added.wait_for(waitLimit), std::future_status::ready);
        const auto result = added.get();
        EXPECT_TRUE(result.has_value()) << result.error().toString();
    }

    TEST_F(OperationQueueBatchTests, ACancelAllWhileTheBatchIsOutLeavesWhatIsQueuedAfterIt)
    {
        const auto held = holdFirstOpen();
        auto queue = makeQueue();
        setPaused(false);
        const auto cancelled = addDownload(*queue, "cancelled.txt");
        addRemoteFile("later.txt");

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        queue->cancelAll();
        const auto later = queueBulkDownload(*queue, {"later.txt"});
        held->release();

        workUntilCompletions(2);
        ASSERT_EQ(completions().size(), 2u);
        EXPECT_EQ(completions().front().operationId, cancelled);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(completions().back().operationId, later);
        EXPECT_EQ(completions().back().reason, SharedData::OperationCompletionReason::Completed);
    }

    TEST_F(OperationQueueBatchTests, ACancelOfAnOperationOutsideTheBatchAppliesWhileTheBatchIsOut)
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
        addDownload(*queue, "held.txt");
        const auto queued = addDownload(*queue, "queued.txt");
        const auto completing = std::make_shared<std::promise<SharedData::OperationCompletionReason>>();
        runOnStrand(
            [&]()
            {
                queue->whenCompleted(
                    queued,
                    [completing](OperationQueue::OperationCompleted const& completed)
                    {
                        completing->set_value(completed.reason);
                    }
                );
            }
        );

        postToStrand(
            [&queue]()
            {
                queue->work();
            }
        );
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);
        queue->cancel(queued);
        auto completed = completing->get_future();
        EXPECT_EQ(completed.wait_for(waitLimit), std::future_status::ready)
            << "the cancel waited for a batch its operation is not in";
        held->release();

        ASSERT_EQ(completed.wait_for(waitLimit), std::future_status::ready);
        EXPECT_EQ(completed.get(), SharedData::OperationCompletionReason::Canceled);
    }

    TEST_F(OperationQueueBatchTests, ABulkInTheBatchOutLeavesWhatItFinishedOutOfItsBackup)
    {
        addRemoteFile("bulk_a.txt");
        addRemoteFile("bulk_b.txt");
        const auto second = remote("/home/test/bulk_b.txt");
        const auto held = holdOpen(
            [&second](RemoteIo::OpenFile const& open)
            {
                return open.path == second;
            }
        );
        auto queue = makeQueue();
        setPaused(false);
        const auto card = queueBulkDownload(*queue, {"bulk_a.txt", "bulk_b.txt"});
        workUntilHeld(*queue, *held);
        ASSERT_EQ(held->hasEntered.wait_for(waitLimit), std::future_status::ready);

        // As a session teardown does, while the bulk is out with its second file.
        BulkResumeRegistry registry;
        std::promise<void> dumping;
        auto dumped = postToStrand(
            [&]()
            {
                dumping.set_value();
                queue->dumpBulkResumes(registry);
            }
        );
        ASSERT_EQ(dumping.get_future().wait_for(waitLimit), std::future_status::ready);
        held->release();
        ASSERT_EQ(dumped.wait_for(waitLimit), std::future_status::ready);

        const auto backup = registry.take(card);
        ASSERT_TRUE(backup.has_value());
        ASSERT_TRUE(std::holds_alternative<SharedData::BulkDownloadRequest>(backup->request));
        auto const& entries = std::get<SharedData::BulkDownloadRequest>(backup->request).entries;
        ASSERT_EQ(entries.size(), 1u) << "the backup holds the finished file too";
        EXPECT_EQ(entries.front().source, second);
    }
}
