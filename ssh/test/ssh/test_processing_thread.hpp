#pragma once

#include <utility/awaiter.hpp>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <gtest/gtest.h>

#include <latch>
#include <thread>

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace SecureShell::Test
{
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

    TEST_F(ProcessingThreadTest, CanPushTaskAfterStop)
    {
        ProcessingThread processingThread;
        processingThread.start(std::chrono::milliseconds{1});
        processingThread.stop();
        EXPECT_TRUE(processingThread.pushTask([] {}));
    }

    TEST_F(ProcessingThreadTest, CannotPushTaskWhileStopping)
    {
        Awaiter awaiter{};
        ProcessingThread processingThread;
        processingThread.pushTask(
            [&]
            {
                awaiter.arrive();
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        );
        std::thread asyncStopper{[&]
            {
                processingThread.stop();
            }};
        awaiter.wait();
        EXPECT_FALSE(processingThread.pushTask([] {}));
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
        bool waitResult = false;
        for (int tryWaitCount = 0; tryWaitCount < 5; ++tryWaitCount)
        {
            if (waitResult = latch.try_wait(); waitResult)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        EXPECT_TRUE(waitResult);
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
            [&processingThread, result, &awaiter](auto)
            {
                processingThread.removePermanentTask(result->second);
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
            [&](auto)
            {
                awaiter.arrive();
                ++counter;
                processingThread.removePermanentTask(result->second);
                return true;
            }
        );

        ASSERT_TRUE(awaiter.waitFor());
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
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

        // Allow several cycles for the buggy replay path to crash the run loop.
        std::this_thread::sleep_for(std::chrono::milliseconds{300});

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
        EXPECT_EQ(0, processingThread.permanentTaskCount());
    }
}