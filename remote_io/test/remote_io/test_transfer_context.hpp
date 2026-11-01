#pragma once

#include <remote_io/async/transfer_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <latch>
#include <thread>

namespace RemoteIo::Test
{
    TEST(TransferContextTests, CancelEndsTheTransferAndWakesItsThread)
    {
        int wakes = 0;
        AsyncTransferContext context{
            [&wakes]()
            {
                ++wakes;
            }
        };

        context.cancel();

        EXPECT_EQ(context.outcome(), AsyncTransferContext::Outcome::Canceled);
        EXPECT_TRUE(context.hasEnded());
        EXPECT_EQ(wakes, 1);
    }

    TEST(TransferContextTests, FailingEndsTheTransferAndWakesItsThread)
    {
        int wakes = 0;
        AsyncTransferContext context{
            [&wakes]()
            {
                ++wakes;
            }
        };

        context.fail(RemoteError{.kind = RemoteErrorKind::ConnectionLost});

        EXPECT_EQ(context.outcome(), AsyncTransferContext::Outcome::Failed);
        EXPECT_EQ(wakes, 1);
    }

    TEST(TransferContextTests, EndingAnEndedTransferDoesNotWakeItsThread)
    {
        int wakes = 0;
        AsyncTransferContext context{
            [&wakes]()
            {
                ++wakes;
            }
        };

        context.complete();
        context.cancel();
        context.fail(RemoteError{.kind = RemoteErrorKind::ConnectionLost});

        EXPECT_EQ(context.outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_EQ(wakes, 0);
    }

    TEST(TransferContextTests, OnlyResumingWakesItsThread)
    {
        int wakes = 0;
        AsyncTransferContext context{
            [&wakes]()
            {
                ++wakes;
            }
        };

        context.pause(true);
        EXPECT_TRUE(context.paused());
        EXPECT_EQ(wakes, 0);

        context.pause(false);
        EXPECT_FALSE(context.paused());
        EXPECT_EQ(wakes, 1);
    }

    TEST(TransferContextTests, AContextWithoutWakePausesAndCancels)
    {
        AsyncTransferContext context{};

        context.pause(true);
        context.cancel();

        EXPECT_TRUE(context.paused());
        EXPECT_EQ(context.outcome(), AsyncTransferContext::Outcome::Canceled);
    }

    TEST(TransferContextTests, TheFirstEndWinsAndKeepsItsError)
    {
        AsyncTransferContext context{};
        context.fail(RemoteError{.kind = RemoteErrorKind::ConnectionLost, .protocol = Protocol::Ftp});
        context.fail(RemoteError{.kind = RemoteErrorKind::TimedOut});
        context.complete();
        context.cancel();

        EXPECT_EQ(context.outcome(), AsyncTransferContext::Outcome::Failed);
        ASSERT_TRUE(context.error().has_value());
        EXPECT_EQ(context.error()->kind, RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(context.error()->protocol, Protocol::Ftp);
    }

    TEST(TransferContextTests, ACancelRacingAFailureLeavesAnErrorOnlyWhenTheFailureWon)
    {
        for (int round = 0; round < 1000; ++round)
        {
            AsyncTransferContext context{};
            std::latch start{2};
            std::thread failing{
                [&context, &start]()
                {
                    start.arrive_and_wait();
                    context.fail(RemoteError{.kind = RemoteErrorKind::ConnectionLost});
                }
            };
            start.arrive_and_wait();
            context.cancel();
            failing.join();

            ASSERT_EQ(context.error().has_value(), context.outcome() == AsyncTransferContext::Outcome::Failed)
                << "round " << round;
        }
    }

    TEST(TransferContextTests, OnlyAFailedTransferHasAnError)
    {
        AsyncTransferContext completed{};
        completed.complete();
        EXPECT_EQ(completed.outcome(), AsyncTransferContext::Outcome::Completed);
        EXPECT_FALSE(completed.error().has_value());

        AsyncTransferContext running{};
        EXPECT_EQ(running.outcome(), AsyncTransferContext::Outcome::Running);
        EXPECT_FALSE(running.error().has_value());
    }

    TEST(TransferContextTests, TheObserverHearsTheEndOnce)
    {
        AsyncTransferContext context{};
        int calls = 0;
        context.observe(
            [&calls]()
            {
                ++calls;
            }
        );

        context.complete();
        context.cancel();

        EXPECT_EQ(calls, 1);
    }

    TEST(TransferContextTests, TheObserverHearsOfProgressAtMostOncePerInterval)
    {
        AsyncTransferContext context{};
        int calls = 0;
        context.observe(
            [&calls]()
            {
                ++calls;
            }
        );

        const auto start = std::chrono::steady_clock::now();
        for (int report = 0; report < 1000; ++report)
            context.reportProgress();
        const auto elapsed = std::chrono::steady_clock::now() - start;

        EXPECT_GE(calls, 1);
        EXPECT_LE(calls, 1 + elapsed / AsyncTransferContext::progressInterval);
    }

    TEST(TransferContextTests, BytesAddUp)
    {
        AsyncTransferContext context{};

        context.addBytesTransferred(100);
        context.addBytesTransferred(28);

        EXPECT_EQ(context.bytesTransferred(), 128);
    }
}
