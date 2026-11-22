#pragma once

#include <backend/sftp/transfer_rate.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace Test
{
    namespace
    {
        constexpr std::uint64_t megabyte = 1'000'000u;

        std::chrono::steady_clock::time_point secondsIn(int seconds)
        {
            return std::chrono::steady_clock::time_point{} + std::chrono::hours{1} + std::chrono::seconds{seconds};
        }
    }

    TEST(TransferRateTests, ThereIsNoRateBeforeTwoSamples)
    {
        TransferRate rate{};

        EXPECT_EQ(rate.sample(50u * megabyte, secondsIn(0)), 0);
        EXPECT_EQ(rate.bytesPerSecond(), 0);
    }

    TEST(TransferRateTests, TheFirstRateIsTakenAsItIs)
    {
        TransferRate rate{};
        rate.sample(0u, secondsIn(0));

        EXPECT_EQ(rate.sample(100u * megabyte, secondsIn(1)), 100 * static_cast<std::int64_t>(megabyte));
    }

    TEST(TransferRateTests, TheRateStaysBetweenSamples)
    {
        TransferRate rate{};
        rate.sample(0u, secondsIn(0));
        rate.sample(100u * megabyte, secondsIn(1));

        const auto early = secondsIn(1) + TransferRate::sampleInterval / 2;
        EXPECT_EQ(rate.sample(400u * megabyte, early), 100 * static_cast<std::int64_t>(megabyte));
    }

    TEST(TransferRateTests, ALaterSampleIsSmoothedIn)
    {
        TransferRate rate{};
        rate.sample(0u, secondsIn(0));
        rate.sample(100u * megabyte, secondsIn(1));

        // 200 MB/s in the second sample: 0.3 * 200 + 0.7 * 100.
        EXPECT_EQ(rate.sample(300u * megabyte, secondsIn(2)), 130 * static_cast<std::int64_t>(megabyte));
    }

    TEST(TransferRateTests, ARestartKeepsAPauseOutOfTheRate)
    {
        TransferRate rate{};
        rate.sample(0u, secondsIn(0));
        rate.sample(100u * megabyte, secondsIn(1));

        rate.restart();
        EXPECT_EQ(rate.bytesPerSecond(), 100 * static_cast<std::int64_t>(megabyte));
        // A minute of pause, then the transfer goes on as fast as before.
        EXPECT_EQ(rate.sample(101u * megabyte, secondsIn(61)), 100 * static_cast<std::int64_t>(megabyte));
        EXPECT_EQ(rate.sample(201u * megabyte, secondsIn(62)), 100 * static_cast<std::int64_t>(megabyte));
    }

    TEST(TransferRateTests, AStallWithoutRestartLowersTheRate)
    {
        TransferRate rate{};
        rate.sample(0u, secondsIn(0));
        rate.sample(100u * megabyte, secondsIn(1));

        EXPECT_EQ(rate.sample(100u * megabyte, secondsIn(2)), 70 * static_cast<std::int64_t>(megabyte));
    }

    TEST(TransferRateTests, ACountThatFellBackCountsAsNothingMoved)
    {
        TransferRate rate{};
        rate.sample(100u * megabyte, secondsIn(0));
        rate.sample(200u * megabyte, secondsIn(1));

        EXPECT_EQ(rate.sample(150u * megabyte, secondsIn(2)), 70 * static_cast<std::int64_t>(megabyte));
    }
}
