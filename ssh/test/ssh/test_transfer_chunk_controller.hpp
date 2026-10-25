#pragma once

#include <ssh/async/transfer_chunk_controller.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace SecureShell::Test
{
    using namespace std::chrono_literals;

    TEST(TransferChunkControllerTests, StartsWithTheMinimumTarget)
    {
        TransferChunkController controller;
        EXPECT_EQ(controller.targetCallDuration(), 50ms);
    }

    TEST(TransferChunkControllerTests, TargetsFourRoundTrips)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(20ms);
        EXPECT_EQ(controller.targetCallDuration(), 80ms);
    }

    TEST(TransferChunkControllerTests, TargetIsCappedForSlowLinks)
    {
        TransferChunkController controller;
        controller.recordRoundTrip(300ms);
        EXPECT_EQ(controller.targetCallDuration(), 250ms);
    }

    TEST(TransferChunkControllerTests, MetadataCallsFollowASlowerLink)
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
