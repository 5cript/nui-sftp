#pragma once

#include <ssh/async/transfer_chunk_controller.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace SecureShell::Test
{
    using namespace std::chrono_literals;

    class TransferChunkControllerTests : public ::testing::Test
    {
      protected:
        static constexpr std::int64_t serverLimit = 256 * 1024;

        /**
         * @brief Runs full-size data calls against a link model: each call costs one round trip
         *        plus the bytes at the given bandwidth.
         */
        static void simulateCalls(
            TransferChunkController& controller,
            std::chrono::nanoseconds roundTrip,
            std::int64_t bytesPerSecond,
            int calls,
            std::int64_t upperBound = serverLimit
        )
        {
            for (int call = 0; call < calls; ++call)
            {
                const auto chunk = controller.preferredTransferChunk(upperBound);
                const auto payload = std::chrono::nanoseconds{chunk * 1'000'000'000 / bytesPerSecond};
                controller.recordDataCall(chunk, chunk, roundTrip + payload);
            }
        }
    };

    TEST_F(TransferChunkControllerTests, StartsPessimisticWithMinimumTarget)
    {
        TransferChunkController controller;
        EXPECT_EQ(controller.preferredTransferChunk(serverLimit), TransferChunkController::initialChunk);
        EXPECT_EQ(controller.targetCallDuration(), 50ms);
    }

    TEST_F(TransferChunkControllerTests, LanGrowsToTheBoundWithoutDriftingTheRoundTrip)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(1ms);
        simulateCalls(controller, 1ms, 100 * 1024 * 1024, 50);

        EXPECT_EQ(controller.preferredTransferChunk(serverLimit), serverLimit);
        EXPECT_EQ(controller.targetCallDuration(), 50ms);
        EXPECT_LE(controller.roundTripNanos(), 1'500'000);
    }

    TEST_F(TransferChunkControllerTests, WanKeepsTargetAtFourRoundTrips)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(20ms);
        // 20 Mbit/s: a full 256 KiB call takes about 125 ms, longer than the 80 ms target.
        simulateCalls(controller, 20ms, 2'500'000, 200);

        EXPECT_LE(controller.roundTripNanos(), 25'000'000) << "data calls must not raise the round trip";
        EXPECT_EQ(controller.targetCallDuration(), 80ms);
        const auto chunk = controller.preferredTransferChunk(serverLimit);
        EXPECT_GE(chunk, 100 * 1024) << "60 ms of payload at 2.5 MB/s";
        EXPECT_LE(chunk, 200 * 1024);
    }

    TEST_F(TransferChunkControllerTests, RoundTripAboveTheTargetCapDoesNotCollapseTheChunk)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(300ms);
        simulateCalls(controller, 300ms, 2'500'000, 50);

        EXPECT_EQ(controller.targetCallDuration(), 250ms);
        EXPECT_GE(controller.preferredTransferChunk(serverLimit), 64 * 1024)
            << "50 ms of payload must survive even though the round trip alone exceeds the target";
    }

    TEST_F(TransferChunkControllerTests, ShortTailCallDoesNotShrinkTheChunk)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(20ms);
        simulateCalls(controller, 20ms, 2'500'000, 50);
        const auto before = controller.preferredTransferChunk(serverLimit);

        controller.recordDataCall(1024, before, 20ms + 1ms);

        EXPECT_EQ(controller.preferredTransferChunk(serverLimit), before);
    }

    TEST_F(TransferChunkControllerTests, DataCallsOnlyLowerTheRoundTrip)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(20ms);

        controller.recordDataCall(64 * 1024, 64 * 1024, 100ms);
        EXPECT_EQ(controller.roundTripNanos(), 20'000'000);

        controller.recordDataCall(64 * 1024, 64 * 1024, 5ms);
        EXPECT_EQ(controller.roundTripNanos(), 5'000'000);
    }

    TEST_F(TransferChunkControllerTests, MetadataCallsFollowASlowerLink)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(5ms);
        for (int call = 0; call < 40; ++call)
            controller.recordRoundTrip(40ms);

        EXPECT_GE(controller.roundTripNanos(), 38'000'000);
        EXPECT_GE(controller.targetCallDuration(), 150ms);
        EXPECT_LE(controller.targetCallDuration(), 160ms);
    }
}
