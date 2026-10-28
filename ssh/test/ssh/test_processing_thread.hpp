#pragma once

#include <utility/awaiter.hpp>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <gtest/gtest.h>

#ifndef _WIN32
#    include <fcntl.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <expected>
#include <functional>
#include <future>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
    namespace
    {
        bool waitUntil(std::function<bool()> const& condition, std::chrono::milliseconds limit = 5s)
        {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (!condition())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return true;
        }
    }

    class ProcessingThreadTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {}

        void TearDown() override
        {}

      protected:
    };

    TEST_F(ProcessingThreadTest, CanStartThreadAndStopIt)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        ASSERT_NO_FATAL_FAILURE(processingThread.stop());
    }

    TEST_F(ProcessingThreadTest, CanStartAndDestructThread)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
    }

    TEST_F(ProcessingThreadTest, CanPushTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.pushTask([] {});
        ASSERT_NO_FATAL_FAILURE(processingThread.stop());
    }

    TEST_F(ProcessingThreadTest, CanPushMultipleTasks)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        for (unsigned int i = 0; i < ProcessingThread::maximumTasksProcessableAtOnce; ++i)
        {
            processingThread.pushTask([] {});
        }
    }

    TEST_F(ProcessingThreadTest, PushedTaskIsEventuallyExecuted)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        EXPECT_TRUE(processingThread.pushTask(
            [&awaiter]
            {
                awaiter.arrive();
            }
        ));
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, AllTasksAreExecutedEvenIfDestructed)
    {
        std::atomic_int counter = 0;
        Awaiter awaiter{};
        {
            ProcessingThread processingThread;
            processingThread.start(std::chrono::milliseconds{1});
            for (unsigned int i = 0; i < ProcessingThread::maximumTasksProcessableAtOnce * 3; ++i)
            {
                EXPECT_TRUE(processingThread.pushTask(
                    [&counter, &awaiter]
                    {
                        if (counter.load() == 0)
                            awaiter.arrive();
                        ++counter;
                    }
                ));
            }
            // wait for at least one task to be executed, otherwise it runs into shutdown immediately and the adds
            // are not processed
            awaiter.wait();
        }
        ASSERT_EQ(ProcessingThread::maximumTasksProcessableAtOnce * 3, counter.load());
    }

    TEST_F(ProcessingThreadTest, CannotPushAfterStopUntilStartedAgain)
    {
        std::atomic_int ran = 0;
        const auto countRun = [&ran]
        {
            ++ran;
        };
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.stop();

        EXPECT_FALSE(processingThread.pushTask(countRun));
        EXPECT_FALSE(processingThread
                         .pushPermanentTask(
                             [](auto)
                             {
                                 return true;
                             }
                         )
                         .first);
        processingThread.stop();
        EXPECT_EQ(ran.load(), 0);

        processingThread.start(std::chrono::milliseconds{1});
        Awaiter awaiter{};
        EXPECT_TRUE(processingThread.pushTask(
            [&awaiter]
            {
                awaiter.arrive();
            }
        ));
        EXPECT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, CannotPushWhileStopWaitsForTheLastTask)
    {
        Awaiter running{};
        std::promise<void> release{};
        const auto released = release.get_future().share();
        std::atomic_int lateRan = 0;
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.pushTask(
            [&running, released]
            {
                running.arrive();
                released.wait_for(5s);
            }
        );
        ASSERT_TRUE(running.waitFor());
        std::thread stopper{[&processingThread]
            {
                processingThread.stop();
            }};
        int accepted = 0;
        const auto refused = waitUntil(
            [&processingThread, &lateRan, &accepted]
            {
                const auto pushed = processingThread.pushTask(
                    [&lateRan]
                    {
                        ++lateRan;
                    }
                );
                if (pushed)
                    ++accepted;
                return !pushed;
            }
        );
        const auto permanentPushed = processingThread
                                         .pushPermanentTask(
                                             [](auto)
                                             {
                                                 return true;
                                             }
                                         )
                                         .first;
        release.set_value();
        stopper.join();

        EXPECT_TRUE(refused);
        EXPECT_FALSE(permanentPushed);
        EXPECT_FALSE(processingThread.pushTask([] {}));
        EXPECT_EQ(lateRan.load(), accepted);
    }

    TEST_F(ProcessingThreadTest, RefusedPromiseTaskCompletesWithTheUnfinishedResult)
    {
        std::atomic_int ran = 0;
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.stop();

        auto refusedBool = processingThread.pushPromiseTask(
            [&ran]
            {
                ++ran;
                return true;
            }
        );
        auto refusedVoid = processingThread.pushPromiseTask(
            [&ran]
            {
                ++ran;
            }
        );
        auto refusedExpected = processingThread.pushPromiseTask(
            [&ran]() -> std::expected<int, std::string>
            {
                ++ran;
                return 1;
            },
            []() -> std::expected<int, std::string>
            {
                return std::unexpected("refused"s);
            }
        );

        ASSERT_EQ(refusedBool.wait_for(0s), std::future_status::ready);
        EXPECT_FALSE(refusedBool.get());
        ASSERT_EQ(refusedVoid.wait_for(0s), std::future_status::ready);
        EXPECT_NO_THROW(refusedVoid.get());
        ASSERT_EQ(refusedExpected.wait_for(0s), std::future_status::ready);
        EXPECT_EQ(refusedExpected.get(), std::unexpected("refused"s));
        EXPECT_FALSE(processingThread.awaitCycle(0ms));
        EXPECT_EQ(ran.load(), 0);
    }

    TEST_F(ProcessingThreadTest, PromiseTaskOfAFinalizedStrandCompletesWithTheUnfinishedResult)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        auto finalTask = strand->pushFinalPromiseTask(
            []
            {
                return true;
            }
        );
        ASSERT_EQ(finalTask.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(finalTask.get());

        auto afterFinal = strand->pushPromiseTask(
            []
            {
                return true;
            }
        );
        auto secondFinal = strand->pushFinalPromiseTask(
            []() -> std::expected<int, std::string>
            {
                return 1;
            },
            []() -> std::expected<int, std::string>
            {
                return std::unexpected("finalized"s);
            }
        );
        ASSERT_EQ(afterFinal.wait_for(0s), std::future_status::ready);
        EXPECT_FALSE(afterFinal.get());
        ASSERT_EQ(secondFinal.wait_for(0s), std::future_status::ready);
        EXPECT_EQ(secondFinal.get(), std::unexpected("finalized"s));
    }

    TEST_F(ProcessingThreadTest, PromiseTaskOfAStrandOnAStoppedThreadCompletesWithTheUnfinishedResult)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        processingThread.stop();

        auto refused = strand->pushPromiseTask(
            []
            {
                return true;
            }
        );
        ASSERT_EQ(refused.wait_for(0s), std::future_status::ready);
        EXPECT_FALSE(refused.get());
    }

    TEST_F(ProcessingThreadTest, ThrowingPromiseTaskCompletesWithTheUnfinishedResult)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto threw = processingThread.pushPromiseTask(
            []() -> bool
            {
                throw std::runtime_error("boom");
            }
        );
        ASSERT_EQ(threw.wait_for(5s), std::future_status::ready);
        EXPECT_FALSE(threw.get());
    }

    TEST_F(ProcessingThreadTest, CannotPushTaskWhileStopping)
    {
        Awaiter awaiter{};
        std::promise<void> release{};
        const auto released = release.get_future().share();
        ProcessingThread processingThread;
        processingThread.pushTask(
            [&awaiter, released]
            {
                awaiter.arrive();
                released.wait_for(5s);
            }
        );
        std::thread asyncStopper{[&]
            {
                processingThread.stop();
            }};
        awaiter.wait();
        EXPECT_FALSE(processingThread.pushTask([] {}));
        release.set_value();
        asyncStopper.join();
    }

    TEST_F(ProcessingThreadTest, PushedTaskIsExecutedOnStopIfNeverRan)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.pushTask(
            [&]
            {
                awaiter.arrive();
            }
        );
        processingThread.stop();
        EXPECT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, CanPushPermanentTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [](auto)
            {
                return true;
            }
        );
        EXPECT_TRUE(result.first);
    }

    TEST_F(ProcessingThreadTest, PermanentTaskIsExecuted)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [&awaiter](auto)
            {
                awaiter.arrive();
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        EXPECT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, PermanentTaskIsExecutedMultipleTimes)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        std::latch latch{5};
        std::atomic_int counter = 5;
        auto result = processingThread.pushPermanentTask(
            [&](auto)
            {
                if (counter.load() == 0)
                    return true;
                latch.count_down();
                --counter;
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        EXPECT_TRUE(waitUntil(
            [&latch]
            {
                return latch.try_wait();
            }
        ));
    }

    TEST_F(ProcessingThreadTest, TaskIsExecutedIfIfPermantentTaskIsRunning)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [](auto)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        processingThread.pushTask(
            [&awaiter]
            {
                awaiter.arrive();
            }
        );
        EXPECT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, TaskIsOnlyExecutedOnceIfPermanentTaskIsRunning)
    {
        std::atomic_int counter = 0;
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [&counter, &awaiter](auto)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                ++counter;
                if (counter.load() == 5)
                    awaiter.arrive();
                return true;
            }
        );
        ASSERT_TRUE(result.first);

        std::atomic_int taskCounter = 0;
        processingThread.pushTask(
            [&taskCounter]
            {
                ++taskCounter;
            }
        );

        ASSERT_TRUE(awaiter.waitFor());
        EXPECT_EQ(1, taskCounter.load());
    }

    TEST_F(ProcessingThreadTest, CanRemovePermanentTaskWhenNotRunning)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [](auto)
            {
                return true;
            }
        );
        processingThread.removePermanentTask(result.second);
    }

    TEST_F(ProcessingThreadTest, CanRemovePermanentTaskWhenRunning)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [&awaiter](auto)
            {
                awaiter.arrive();
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        ASSERT_TRUE(awaiter.waitFor());
        ASSERT_NO_FATAL_FAILURE(processingThread.removePermanentTask(result.second));
    }

    TEST_F(ProcessingThreadTest, CanRemovePermanentTaskFromRegularTask)
    {
        Awaiter awaiter{};
        Awaiter awaiter2{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [&awaiter](auto)
            {
                awaiter.arrive();
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        ASSERT_TRUE(awaiter.waitFor());
        std::atomic_bool removeResult = false;
        processingThread.pushTask(
            [&processingThread, result, &removeResult, &awaiter2]
            {
                removeResult = processingThread.removePermanentTask(result.second);
                awaiter2.arrive();
            }
        );
        ASSERT_TRUE(awaiter2.waitFor());
        EXPECT_TRUE(removeResult);
        processingThread.stop();
        awaiter.reset();
        processingThread.start(std::chrono::milliseconds{1});
        EXPECT_FALSE(awaiter.waitFor(400ms));
    }

    TEST_F(ProcessingThreadTest, CanAddPermanentTaskFromRegularTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        std::atomic_bool pushResult = false;
        processingThread.pushTask(
            [&processingThread, &awaiter, &pushResult]
            {
                pushResult = processingThread
                                 .pushPermanentTask(
                                     [&awaiter](auto)
                                     {
                                         awaiter.arrive();
                                         return true;
                                     }
                                 )
                                 .first;
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
        EXPECT_TRUE(pushResult);
    }

    TEST_F(ProcessingThreadTest, CanAddRegularTaskFromRegularTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        std::atomic_bool pushResult = false;
        processingThread.pushTask(
            [&processingThread, &awaiter, &pushResult]
            {
                pushResult = processingThread.pushTask(
                    [&awaiter]
                    {
                        awaiter.arrive();
                    }
                );
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
        EXPECT_TRUE(pushResult);
    }

    TEST_F(ProcessingThreadTest, CanRemovePermanentTaskFromPermanentTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        std::shared_ptr<std::pair<bool, ProcessingThread::PermanentTaskId>> result =
            std::make_shared<std::pair<bool, ProcessingThread::PermanentTaskId>>();
        *result = processingThread.pushPermanentTask(
            [&processingThread, result, &awaiter](auto id)
            {
                processingThread.removePermanentTask(id);
                awaiter.arrive();
                /*
                 * DONT DO ANYTHING HERE WITH CAPTURES, AS THEY ARE DESTROYED
                 */
                return true;
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
        processingThread.stop();
        EXPECT_EQ(0, processingThread.permanentTaskCount());
    }

    TEST_F(ProcessingThreadTest, RemovedPermanentTaskIsNoLongerCalled)
    {
        Awaiter awaiter{};
        std::atomic_int counter = 0;
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        std::shared_ptr<std::pair<bool, ProcessingThread::PermanentTaskId>> result =
            std::make_shared<std::pair<bool, ProcessingThread::PermanentTaskId>>();
        *result = processingThread.pushPermanentTask(
            [&](auto id)
            {
                awaiter.arrive();
                ++counter;
                processingThread.removePermanentTask(id);
                return true;
            }
        );

        ASSERT_TRUE(awaiter.waitFor());
        // The second awaited cycle ran every permanent task once more.
        ASSERT_TRUE(processingThread.awaitCycle());
        ASSERT_TRUE(processingThread.awaitCycle());
        processingThread.stop();
        EXPECT_EQ(1, counter.load());
    }

    TEST_F(ProcessingThreadTest, CanSafelyDoubleDeletePermanentTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto result = processingThread.pushPermanentTask(
            [](auto)
            {
                return true;
            }
        );
        EXPECT_TRUE(processingThread.removePermanentTask(result.second));
        EXPECT_FALSE(processingThread.removePermanentTask(result.second));
    }

    TEST_F(ProcessingThreadTest, CanClearPermanentTasks)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.pushPermanentTask(
            [](auto)
            {
                return true;
            }
        );
        processingThread.clearPermanentTasks();
        EXPECT_EQ(0, processingThread.permanentTaskCount());
    }

    TEST_F(ProcessingThreadTest, CanWaitForCycle)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        EXPECT_TRUE(processingThread.awaitCycle());
    }

    TEST_F(ProcessingThreadTest, CanWaitForCyclesMultipleTimes)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        for (int i = 0; i < 5; ++i)
        {
            EXPECT_TRUE(processingThread.awaitCycle());
        }
    }

    TEST_F(ProcessingThreadTest, CanMakeStrandAndPushTaskThroughIt)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushTask([] {});
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, CanMakeStrandAndPushPermanentTaskThroughIt)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushPermanentTask(
            []()
            {
                return true;
            }
        );
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, TaskInStrandIsExecuted)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushTask(
            [&awaiter]
            {
                awaiter.arrive();
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, PermanentTaskInStrandIsExecuted)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        auto result = strand->pushPermanentTask(
            [&awaiter]()
            {
                awaiter.arrive();
                return true;
            }
        );
        ASSERT_TRUE(result.first);
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, CannotPushTaskAfterFinalTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushFinalTask([] {});
        EXPECT_FALSE(strand->pushTask([] {}));
    }

    TEST_F(ProcessingThreadTest, CannotPushPermanentTaskAfterFinalTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushFinalTask([] {});
        EXPECT_FALSE(strand
                ->pushPermanentTask(
                    []
                    {
                        return true;
                    }
                )
                .first);
    }

    TEST_F(ProcessingThreadTest, CanPushFinalTaskAfterFinalTask)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushFinalTask([] {});
        EXPECT_NO_THROW(strand->pushFinalTask([] {}));
    }

    TEST_F(ProcessingThreadTest, FinalTaskRemovesAllPermanentTasks)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushPermanentTask(
            []
            {
                return true;
            }
        );
        strand->pushPermanentTask(
            []
            {
                return true;
            }
        );
        strand->pushPermanentTask(
            []
            {
                return true;
            }
        );
        strand->pushFinalTask([] {});
        EXPECT_EQ(0, processingThread.permanentTaskCount());
    }

    TEST_F(ProcessingThreadTest, CanPushFinalFromTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushTask(
            [&strand, &awaiter]
            {
                strand->pushFinalTask(
                    [&awaiter]
                    {
                        awaiter.arrive();
                    }
                );
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, CanPushFinalFromPermanentTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushPermanentTask(
            [&strand, &awaiter]
            {
                strand->pushFinalTask(
                    [&awaiter]
                    {
                        awaiter.arrive();
                    }
                );
                return true;
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, CanPushFinalFromTaskFromPermanentTask)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        strand->pushPermanentTask(
            [&strand, &awaiter]
            {
                strand->pushTask(
                    [&strand, &awaiter]
                    {
                        strand->pushFinalTask(
                            [&awaiter]
                            {
                                awaiter.arrive();
                            }
                        );
                    }
                );
                return true;
            }
        );
        ASSERT_TRUE(awaiter.waitFor());
    }

    TEST_F(ProcessingThreadTest, ExternalRemovePermanentTaskWhileProcessingDoesNotKillThread)
    {
        // Regression: while the processing thread iterated permanent tasks, an external
        // call to removePermanentTask enqueued a promise-fulfilling lambda into
        // deferredTaskModification_. The vector was never cleared after being drained, so
        // the same lambda replayed on the next cycle, throwing future_error from the
        // already-satisfied promise. The outer catch then set running_ = false and the
        // thread silently died, deadlocking any subsequent .get() on a queued task.
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});

        Awaiter blockerStarted{};
        auto blocker = processingThread.pushPermanentTask(
            [&blockerStarted](auto)
            {
                blockerStarted.arrive();
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                return true;
            }
        );
        ASSERT_TRUE(blocker.first);

        auto victim = processingThread.pushPermanentTask(
            [](auto)
            {
                return true;
            }
        );
        ASSERT_TRUE(victim.first);

        ASSERT_TRUE(blockerStarted.waitFor());

        EXPECT_TRUE(processingThread.removePermanentTask(victim.second));

        // Several cycles for the buggy replay path to crash the run loop.
        for (int cycle = 0; cycle < 3; ++cycle)
            processingThread.awaitCycle();

        Awaiter postRemoval{};
        EXPECT_TRUE(processingThread.pushTask(
            [&postRemoval]
            {
                postRemoval.arrive();
            }
        ));
        EXPECT_TRUE(postRemoval.waitFor());
    }

    TEST_F(ProcessingThreadTest, PermanentTaskReturningFalseIsRemoved)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        auto result = strand->pushPermanentTask(
            []
            {
                return false;
            }
        );
        ASSERT_TRUE(result.first);
        Awaiter awaiter{};
        strand->pushTask(
            [&awaiter]
            {
                awaiter.arrive();
            }
        );
        awaiter.wait();
        // The task may have run in a cycle already past its permanent tasks, the next cycle runs them.
        ASSERT_TRUE(processingThread.awaitCycle());
        EXPECT_EQ(0, processingThread.permanentTaskCount());
    }

    TEST_F(ProcessingThreadTest, StrandFinalizesAfterSelfRemovingPermanentTask)
    {
        // The strand wrapper no longer erases self-removed ids from the strand's set; the
        // stale id lingers until finalization, where ProcessingThread::removePermanentTask
        // is a no-op for unknown ids. This test exercises that path end-to-end.
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();

        Awaiter selfRemoved{};
        auto result = strand->pushPermanentTask(
            [&selfRemoved]
            {
                selfRemoved.arrive();
                return false;
            }
        );
        ASSERT_TRUE(result.first);
        ASSERT_TRUE(selfRemoved.waitFor());

        // Confirm the thread side dropped the task.
        Awaiter sync{};
        strand->pushTask(
            [&sync]
            {
                sync.arrive();
            }
        );
        ASSERT_TRUE(sync.waitFor());
        EXPECT_EQ(0, processingThread.permanentTaskCount());

        // Strand still holds the stale id; finalization must complete cleanly.
        Awaiter finalRan{};
        strand->pushFinalTask(
            [&finalRan]
            {
                finalRan.arrive();
            }
        );
        EXPECT_TRUE(finalRan.waitFor());
    }

    TEST_F(ProcessingThreadTest, PendingTasksAtStopAreDrainedInFifoOrder)
    {
        std::vector<int> order;
        std::mutex orderMutex;

        ProcessingThread processingThread;
        for (int i = 0; i < 5; ++i)
        {
            processingThread.pushTask(
                [i, &order, &orderMutex]
                {
                    std::lock_guard lock{orderMutex};
                    order.push_back(i);
                }
            );
        }
        processingThread.stop();

        EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
    }

    TEST_F(ProcessingThreadTest, ThrowingPermanentTaskRunsRemainingBatchAndStopsThread)
    {
        std::atomic_int throwerCount = 0;
        std::atomic_int goodCount = 0;

        ProcessingThread processingThread;
        processingThread.pushPermanentTask(
            [&throwerCount](auto)
            {
                ++throwerCount;
                throw std::runtime_error("boom");
                return true;
            }
        );
        processingThread.pushPermanentTask(
            [&goodCount](auto)
            {
                ++goodCount;
                return true;
            }
        );
        processingThread.start(std::chrono::milliseconds{1});

        // The run loop exits via the rethrow once the cycle completed.
        ASSERT_TRUE(waitUntil(
            [&processingThread]
            {
                return !processingThread.isRunning();
            }
        ));

        EXPECT_EQ(1, throwerCount.load());   // thrower not re-run
        EXPECT_EQ(1, goodCount.load());      // remaining task in batch still ran
        EXPECT_FALSE(processingThread.isRunning());
    }

    TEST_F(ProcessingThreadTest, ThrowingRegularTaskRunsRemainingBatchAndStopsThread)
    {
        std::atomic_int firstRan = 0;
        std::atomic_int thirdRan = 0;

        ProcessingThread processingThread;
        processingThread.pushTask(
            [&firstRan]
            {
                firstRan = 1;
            }
        );
        processingThread.pushTask(
            []
            {
                throw std::runtime_error("boom");
            }
        );
        processingThread.pushTask(
            [&thirdRan]
            {
                thirdRan = 1;
            }
        );
        processingThread.start(std::chrono::milliseconds{1});

        ASSERT_TRUE(waitUntil(
            [&processingThread]
            {
                return !processingThread.isRunning();
            }
        ));

        EXPECT_EQ(1, firstRan.load());
        EXPECT_EQ(1, thirdRan.load());       // task after thrower in same batch still ran
        EXPECT_FALSE(processingThread.isRunning());
    }

    TEST_F(ProcessingThreadTest, StartingTwiceWithoutStopThrows)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        EXPECT_THROW(processingThread.start(std::chrono::milliseconds{1}), std::logic_error);
    }

    TEST_F(ProcessingThreadTest, PermanentTasksStartAtARotatingPositionEachCycle)
    {
        // Tasks that skip their turn once a cycle ran long rely on the start position moving,
        // otherwise the last tasks in the map would never run. Recorded without a lock so no
        // task can block long enough to count as the cycle's blocking task.
        constexpr std::size_t recorded = 30;
        std::array<std::atomic<int>, recorded> order{};
        std::atomic<std::size_t> count{0};
        ProcessingThread processingThread;
        for (int index = 0; index < 3; ++index)
        {
            const auto result = processingThread.pushPermanentTask(
                [index, &order, &count](auto)
                {
                    const auto slot = count.fetch_add(1);
                    if (slot < recorded)
                        order[slot] = index;
                    return true;
                }
            );
            ASSERT_TRUE(result.first);
        }
        processingThread.start(std::chrono::milliseconds{1});
        waitUntil(
            [&count]
            {
                return count.load() >= recorded;
            }
        );
        processingThread.stop();

        ASSERT_GE(count.load(), recorded);
        // Each cycle runs all three tasks in map order from its start position, and the start
        // position must not stay put. A preempted last task moves it by a whole cycle, so only some cycles move it.
        std::vector<int> starts;
        for (std::size_t cycle = 0; cycle < recorded; cycle += 3)
        {
            const int start = order[cycle];
            EXPECT_EQ(order[cycle + 1], (start + 1) % 3);
            EXPECT_EQ(order[cycle + 2], (start + 2) % 3);
            starts.push_back(start);
        }
        EXPECT_NE(std::ranges::count(starts, starts.front()), static_cast<std::ptrdiff_t>(starts.size()));
    }

    namespace
    {
        using TaskStatus = ProcessingThread::PermanentTaskStatus;

        /**
         * @brief A permanent task that counts its turns and answers with a fixed status.
         */
        std::function<TaskStatus(ProcessingThread::PermanentTaskId)> countingTask(std::atomic_int& turns, TaskStatus status)
        {
            return [&turns, status](auto)
            {
                ++turns;
                return status;
            };
        }

    }

    TEST_F(ProcessingThreadTest, IdleTaskLetsTheThreadSleepUntilWoken)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Idle));
        processingThread.start(1ms);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));

        const auto asleep = turns.load();
        std::this_thread::sleep_for(200ms);
        EXPECT_EQ(turns.load(), asleep);

        processingThread.wake();
        ASSERT_TRUE(waitUntil(
            [&turns, asleep]
            {
                return turns.load() > asleep;
            }
        ));
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(turns.load(), asleep + 1);
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, ThreadWithoutTasksDoesNotCycle)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.start(1ms);
        std::this_thread::sleep_for(50ms);
        // A permanent task pushed later must still run right away.
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Idle));
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() == 1;
            }
        ));
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, PollingTaskRunsAboutOncePerItsInterval)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Poll), 20ms);
        const auto start = std::chrono::steady_clock::now();
        processingThread.start(1ms);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 10;
            }
        ));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        processingThread.stop();

        // The thread never wakes early, a slow machine only adds. Polling at its own 1 ms would take about 10 ms.
        EXPECT_GE(elapsed, 9 * 20ms);
    }

    TEST_F(ProcessingThreadTest, ShortestPollIntervalDecidesTheSleep)
    {
        std::atomic_int slowTurns = 0;
        std::atomic_int fastTurns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(slowTurns, TaskStatus::Poll), 10s);
        processingThread.pushPermanentTask(countingTask(fastTurns, TaskStatus::Poll), 10ms);
        processingThread.start(10s);
        // Sleeping for the longest interval would take 40 s.
        ASSERT_TRUE(waitUntil(
            [&fastTurns]
            {
                return fastTurns.load() >= 5;
            }
        ));
        // Read between cycles, a stop could land between the two tasks of one.
        const auto [slow, fast] = processingThread
                                      .pushPromiseTask(
                                          [&slowTurns, &fastTurns]
                                          {
                                              return std::pair{slowTurns.load(), fastTurns.load()};
                                          },
                                          []
                                          {
                                              return std::pair{-1, -2};
                                          }
                                      )
                                      .get();
        processingThread.stop();

        EXPECT_EQ(slow, fast);
    }

    TEST_F(ProcessingThreadTest, BusyTaskKeepsTheThreadFromSleeping)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Busy));
        processingThread.start(10s);
        // Sleeping between busy turns would take 200 s.
        EXPECT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() > 20;
            }
        ));
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, PushedTaskWakesASleepingThreadAtOnce)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Poll));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));

        const auto start = std::chrono::steady_clock::now();
        auto done = processingThread.pushPromiseTask(
            []
            {
                return 42;
            },
            []
            {
                return 0;
            }
        );
        ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(done.get(), 42);
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, StopWakesASleepingThread)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Poll));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));

        const auto start = std::chrono::steady_clock::now();
        processingThread.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
        EXPECT_FALSE(processingThread.isRunning());
    }

    TEST_F(ProcessingThreadTest, DestroyingASleepingThreadReturns)
    {
        std::atomic_int ran = 0;
        {
            ProcessingThread processingThread;
            processingThread.start(10s);
            processingThread.pushTask(
                [&ran]
                {
                    ++ran;
                }
            );
            ASSERT_TRUE(waitUntil(
                [&ran]
                {
                    return ran.load() == 1;
                }
            ));
        }
        EXPECT_EQ(ran.load(), 1);
    }

    TEST_F(ProcessingThreadTest, EveryAcceptedTaskRunsBeforeStopReturnsWhenPushesRace)
    {
        constexpr int pushers = 4;
        std::atomic_int accepted = 0;
        std::atomic_int ran = 0;
        std::atomic_bool keepPushing = true;
        bool pushing = false;
        int ranWhenStopped = 0;
        bool refusedAfterStop = false;
        {
            ProcessingThread processingThread;
            processingThread.start(1ms);
            std::vector<std::thread> threads;
            for (int pusher = 0; pusher < pushers; ++pusher)
            {
                threads.emplace_back(
                    [&]
                    {
                        while (keepPushing)
                        {
                            if (processingThread.pushTask(
                                    [&ran]
                                    {
                                        ++ran;
                                    }
                                ))
                            {
                                ++accepted;
                            }
                            processingThread.wake();
                        }
                    }
                );
            }
            pushing = waitUntil(
                [&accepted]
                {
                    return accepted.load() >= 1000;
                }
            );
            processingThread.stop();
            ranWhenStopped = ran.load();
            refusedAfterStop = !processingThread.pushTask([] {});
            keepPushing = false;
            for (auto& thread : threads)
                thread.join();
        }
        EXPECT_TRUE(pushing);
        EXPECT_TRUE(refusedAfterStop);
        // A push that passed the check before stop() but queued after its drain would run only in the destructor.
        EXPECT_EQ(ranWhenStopped, accepted.load());
        EXPECT_EQ(ran.load(), accepted.load());
    }

    TEST_F(ProcessingThreadTest, ThrowingTaskEndsTheThreadEvenWhileOthersPoll)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::Poll), 10s);
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));
        processingThread.pushTask(
            []
            {
                throw std::runtime_error("boom");
            }
        );
        EXPECT_TRUE(waitUntil(
            [&processingThread]
            {
                return !processingThread.isRunning();
            }
        ));
    }

    TEST_F(ProcessingThreadTest, PushesAfterATaskThrewAreRefusedAndQueuedTasksFail)
    {
        std::atomic_int ran = 0;
        std::future<bool> queuedBehind{};
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        // Queued from within, the task waits for the next cycle the loop does not reach.
        processingThread.pushTask(
            [&processingThread, &queuedBehind, &ran]
            {
                queuedBehind = processingThread.pushPromiseTask(
                    [&ran]
                    {
                        ++ran;
                        return true;
                    }
                );
                throw std::runtime_error("boom");
            }
        );
        ASSERT_TRUE(waitUntil(
            [&processingThread]
            {
                return !processingThread.isRunning();
            }
        ));
        ASSERT_EQ(queuedBehind.wait_for(5s), std::future_status::ready);
        EXPECT_FALSE(queuedBehind.get());

        EXPECT_FALSE(processingThread.pushTask(
            [&ran]
            {
                ++ran;
            }
        ));
        EXPECT_FALSE(processingThread
                         .pushPermanentTask(
                             [](auto)
                             {
                                 return true;
                             }
                         )
                         .first);
        auto refused = processingThread.pushPromiseTask(
            [&ran]
            {
                ++ran;
                return true;
            }
        );
        ASSERT_EQ(refused.wait_for(0s), std::future_status::ready);
        EXPECT_FALSE(refused.get());
        processingThread.stop();
        EXPECT_EQ(ran.load(), 0);
    }

    TEST_F(ProcessingThreadTest, CleanupTasksStillRunAfterATaskThrew)
    {
        std::atomic_bool queuedRanOnTheThread = false;
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.pushTask(
            [&processingThread, &queuedRanOnTheThread]
            {
                processingThread.pushCleanupTask(
                    [&processingThread, &queuedRanOnTheThread]
                    {
                        queuedRanOnTheThread = processingThread.withinProcessingThread();
                    }
                );
                throw std::runtime_error("boom");
            }
        );
        ASSERT_TRUE(waitUntil(
            [&processingThread]
            {
                return !processingThread.isRunning();
            }
        ));

        // Nothing else runs it now, so it runs here before the push returns, acting as the processing thread.
        auto later = processingThread.pushCleanupPromiseTask(
            [&processingThread]
            {
                return processingThread.withinProcessingThread();
            }
        );
        ASSERT_EQ(later.wait_for(0s), std::future_status::ready);
        EXPECT_TRUE(later.get());
        EXPECT_FALSE(processingThread.withinProcessingThread());
        EXPECT_TRUE(queuedRanOnTheThread.load());
    }

    TEST_F(ProcessingThreadTest, FinalTaskAfterStopRunsOnTheCallerAndMayDestroyItsStrand)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        auto strand = processingThread.createStrand();
        auto* const finalizing = strand.get();
        processingThread.stop();

        auto closed = finalizing->pushFinalPromiseTask(
            [&strand, &processingThread]
            {
                const auto within = processingThread.withinProcessingThread();
                strand.reset();
                return within;
            }
        );
        ASSERT_EQ(closed.wait_for(0s), std::future_status::ready);
        EXPECT_TRUE(closed.get());
        EXPECT_EQ(strand, nullptr);
        EXPECT_FALSE(processingThread.withinProcessingThread());
    }

    TEST_F(ProcessingThreadTest, NoTaskRunsWhileItsOwnerIsDestroyed)
    {
        // Members after the thread are destroyed before it, like the libssh session and channels of a Session.
        struct Owner
        {
            ProcessingThread processingThread{};
            std::vector<int> touched{};
            std::function<void()> afterStop{};

            ~Owner()
            {
                processingThread.stop();
                afterStop();
            }
        };

        std::atomic_bool stopped = false;
        std::atomic_bool keepPushing = true;
        std::atomic_int pushedAfterStop = 0;
        std::atomic_int acceptedAfterStop = 0;
        std::atomic_int ranAfterStop = 0;
        auto owner = std::make_unique<Owner>();
        auto* const raw = owner.get();
        raw->processingThread.start(std::chrono::milliseconds{1});
        std::thread pusher{[&]
            {
                while (keepPushing)
                {
                    const auto afterStop = stopped.load();
                    const auto accepted = raw->processingThread.pushTask(
                        [raw, &stopped, &ranAfterStop]
                        {
                            if (stopped)
                                ++ranAfterStop;
                            raw->touched.push_back(1);
                        }
                    );
                    if (afterStop)
                    {
                        ++pushedAfterStop;
                        if (accepted)
                            ++acceptedAfterStop;
                    }
                }
            }};
        raw->afterStop = [&]
        {
            stopped = true;
            waitUntil(
                [&pushedAfterStop]
                {
                    return pushedAfterStop.load() >= 100;
                }
            );
            keepPushing = false;
            pusher.join();
        };

        owner.reset();
        EXPECT_GE(pushedAfterStop.load(), 100);
        EXPECT_EQ(acceptedAfterStop.load(), 0);
        EXPECT_EQ(ranAfterStop.load(), 0);
    }

    TEST_F(ProcessingThreadTest, StrandWakeRunsAnIdleTaskAgain)
    {
        std::atomic_int turns = 0;
        std::atomic_bool hasWork = false;
        ProcessingThread processingThread;
        auto strand = processingThread.createStrand();
        strand->pushPermanentTask(
            std::function<TaskStatus()>{
                [&turns, &hasWork]
                {
                    ++turns;
                    return hasWork.exchange(false) ? TaskStatus::Busy : TaskStatus::Idle;
                }
            }
        );
        processingThread.start(1ms);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));
        std::this_thread::sleep_for(50ms);
        const auto asleep = turns.load();

        hasWork = true;
        strand->wake();
        // One turn finds the work, the busy answer gives it one more turn, then it sleeps again.
        ASSERT_TRUE(waitUntil(
            [&turns, asleep]
            {
                return turns.load() >= asleep + 2;
            }
        ));
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(turns.load(), asleep + 2);
        processingThread.stop();
    }

#ifndef _WIN32
    namespace
    {
        /**
         * @brief A connected pair of local sockets; the thread watches the first, the test writes to the second.
         */
        struct SocketPair
        {
            int ends[2]{-1, -1};

            SocketPair()
            {
                EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends), 0);
                EXPECT_EQ(::fcntl(ends[0], F_SETFL, O_NONBLOCK), 0);
            }
            ~SocketPair()
            {
                for (const auto end : ends)
                {
                    if (end >= 0)
                        ::close(end);
                }
            }
            SocketPair(SocketPair const&) = delete;
            SocketPair& operator=(SocketPair const&) = delete;

            void send()
            {
                const char byte = 'x';
                EXPECT_EQ(::write(ends[1], &byte, 1), 1);
            }

            int drain()
            {
                char buffer[64];
                int total = 0;
                for (auto bytes = ::read(ends[0], buffer, sizeof(buffer)); bytes > 0;
                     bytes = ::read(ends[0], buffer, sizeof(buffer)))
                {
                    total += static_cast<int>(bytes);
                }
                return total;
            }
        };

        /**
         * @brief A permanent task that reads what the pair brought and then awaits input, like a channel reader.
         */
        std::function<TaskStatus(ProcessingThread::PermanentTaskId)>
        readingTask(SocketPair& pair, std::atomic_int& turns, std::atomic_int& received)
        {
            return [&pair, &turns, &received](auto)
            {
                ++turns;
                received += pair.drain();
                return TaskStatus::AwaitInput;
            };
        }

        SocketWait::Watched watching(int socket, bool writable = false)
        {
            return SocketWait::Watched{.socket = socket, .writable = writable};
        }
    }

    TEST_F(ProcessingThreadTest, TaskAwaitingInputSleepsUntilTheSocketHasData)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair]
            {
                return watching(pair.ends[0]);
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.start(1ms);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));

        std::this_thread::sleep_for(200ms);
        const auto asleep = turns.load();
        // Polling every millisecond would have run about 200 times.
        EXPECT_LE(asleep, 2);

        pair.send();
        ASSERT_TRUE(waitUntil(
            [&received]
            {
                return received.load() == 1;
            }
        ));
        std::this_thread::sleep_for(100ms);
        EXPECT_LE(turns.load(), asleep + 2);
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, PushedTaskInterruptsTheSocketWait)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair]
            {
                return watching(pair.ends[0]);
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));
        std::this_thread::sleep_for(50ms);

        const auto start = std::chrono::steady_clock::now();
        auto done = processingThread.pushPromiseTask(
            []
            {
                return 42;
            },
            []
            {
                return 0;
            }
        );
        ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(done.get(), 42);
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, WakeAndNewPermanentTasksInterruptTheSocketWait)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        std::atomic_int otherTurns = 0;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair]
            {
                return watching(pair.ends[0]);
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));
        std::this_thread::sleep_for(50ms);

        const auto beforeWake = turns.load();
        processingThread.wake();
        EXPECT_TRUE(waitUntil(
            [&turns, beforeWake]
            {
                return turns.load() > beforeWake;
            }
        ));
        std::this_thread::sleep_for(50ms);
        processingThread.pushPermanentTask(countingTask(otherTurns, TaskStatus::Done));
        EXPECT_TRUE(waitUntil(
            [&otherTurns]
            {
                return otherTurns.load() == 1;
            }
        ));
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, StopInterruptsTheSocketWait)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair]
            {
                return watching(pair.ends[0]);
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));
        std::this_thread::sleep_for(50ms);

        const auto start = std::chrono::steady_clock::now();
        processingThread.stop();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
        EXPECT_FALSE(processingThread.isRunning());
    }

    TEST_F(ProcessingThreadTest, TaskAwaitingInputPollsWhenTheSocketIsGone)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        // Libssh reports no socket once it closed it after a fatal error.
        ASSERT_TRUE(processingThread.watchSocket(
            []
            {
                return watching(SSH_INVALID_SOCKET);
            }
        ));
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::AwaitInput), 5ms);
        processingThread.start(10s);
        EXPECT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 5;
            }
        ));
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, TaskAwaitingInputPollsWithoutAWatchedSocket)
    {
        std::atomic_int turns = 0;
        ProcessingThread processingThread;
        processingThread.pushPermanentTask(countingTask(turns, TaskStatus::AwaitInput), 5ms);
        processingThread.start(10s);
        EXPECT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 5;
            }
        ));
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, PendingOutputWakesTheThreadWhenTheSocketTakesIt)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        std::atomic_bool outputPending = false;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair, &outputPending]
            {
                return watching(pair.ends[0], outputPending.load());
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.start(10s);
        ASSERT_TRUE(waitUntil(
            [&turns]
            {
                return turns.load() >= 1;
            }
        ));

        // The socket can take data, so while libssh holds output the thread keeps going to flush it.
        outputPending = true;
        processingThread.wake();
        const auto pending = turns.load();
        EXPECT_TRUE(waitUntil(
            [&turns, pending]
            {
                return turns.load() >= pending + 10;
            }
        ));
        outputPending = false;
        processingThread.wake();
        std::this_thread::sleep_for(50ms);
        const auto flushed = turns.load();
        std::this_thread::sleep_for(100ms);
        // A late thread may still take the turns of the last wake, a spinning one takes hundreds.
        EXPECT_LE(turns.load(), flushed + 5);
        processingThread.stop();
    }

    TEST_F(ProcessingThreadTest, PollingTaskStillRunsWhileAnotherAwaitsInput)
    {
        SocketPair pair;
        std::atomic_int turns = 0;
        std::atomic_int received = 0;
        std::atomic_int pollTurns = 0;
        ProcessingThread processingThread;
        ASSERT_TRUE(processingThread.watchSocket(
            [&pair]
            {
                return watching(pair.ends[0]);
            }
        ));
        processingThread.pushPermanentTask(readingTask(pair, turns, received));
        processingThread.pushPermanentTask(countingTask(pollTurns, TaskStatus::Poll), 10ms);
        processingThread.start(10s);
        // The socket wait ends at the polling task's interval.
        EXPECT_TRUE(waitUntil(
            [&pollTurns]
            {
                return pollTurns.load() >= 5;
            }
        ));
        processingThread.stop();
    }
#endif
}