#pragma once

#include <ssh/async/request_window.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace SecureShell::Test
{
    using namespace std::chrono_literals;

    class RequestWindowTests : public ::testing::Test
    {
      protected:
        static constexpr std::int64_t serverLimit = 255 * 1024;
        static constexpr std::int64_t mebibyte = 1024 * 1024;

        /**
         * @brief Completes requests against a link model: the window delivers a window per round
         *        trip until the bandwidth is the limit.
         *
         * @return When the last request completed.
         */
        static std::chrono::steady_clock::time_point simulate(
            RequestWindow& window,
            std::chrono::nanoseconds roundTrip,
            std::int64_t bytesPerSecond,
            std::chrono::steady_clock::duration target,
            int completions,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + 1s
        )
        {
            for (int completion = 0; completion < completions; ++completion)
            {
                const auto request = window.requestBytes(serverLimit);
                const auto latencyBound = window.windowBytes() * 1'000'000'000 / roundTrip.count();
                const auto rate = std::min(bytesPerSecond, latencyBound);
                now += std::chrono::nanoseconds{request * 1'000'000'000 / rate};
                window.recordReceived(request, now, target);
            }
            return now;
        }
    };

    TEST_F(RequestWindowTests, StartsWithEightRequestsOfTheInitialWindow)
    {
        const RequestWindow window;
        EXPECT_EQ(window.windowBytes(), RequestWindow::initialWindow);
        EXPECT_EQ(window.requestBytes(serverLimit), RequestWindow::initialWindow / RequestWindow::requestsPerWindow);
    }

    TEST_F(RequestWindowTests, FastLanGrowsToTheMaximumWithRequestsAtTheServerLimit)
    {
        RequestWindow window;
        simulate(window, 100us, 650 * mebibyte, 50ms, 5000);

        EXPECT_EQ(window.windowBytes(), RequestWindow::maximumWindow);
        EXPECT_EQ(window.requestBytes(serverLimit), serverLimit);
    }

    TEST_F(RequestWindowTests, LatencyBoundWindowGrowsPastTheBandwidthDelayProduct)
    {
        RequestWindow window;
        const auto bandwidth = 100 * mebibyte;
        simulate(window, 50ms, bandwidth, 200ms, 3000);

        // 5 MiB are in flight on this link at full speed.
        EXPECT_GE(window.windowBytes(), bandwidth / 20);
    }

    TEST_F(RequestWindowTests, SlowLinkDrainsWithinTheTargetDuration)
    {
        RequestWindow window;
        const auto bandwidth = 1 * mebibyte;
        simulate(window, 20ms, bandwidth, 80ms, 500);

        // Draining the window takes about one target duration, not seconds.
        EXPECT_LE(window.windowBytes(), bandwidth * 100 / 1000);
        EXPECT_GE(window.windowBytes(), RequestWindow::minimumWindow);
        EXPECT_LT(window.requestBytes(serverLimit), serverLimit);
    }

    TEST_F(RequestWindowTests, RestartedSampleDoesNotCountAPauseAsASlowLink)
    {
        RequestWindow window;
        const auto pausedAt = simulate(window, 100us, 650 * mebibyte, 50ms, 5000);
        window.restartSample();
        simulate(window, 100us, 650 * mebibyte, 50ms, 10, pausedAt + 10s);

        EXPECT_EQ(window.windowBytes(), RequestWindow::maximumWindow);
    }

    TEST_F(RequestWindowTests, RequestsStayWithinASmallUpperBound)
    {
        const RequestWindow window;
        EXPECT_EQ(window.requestBytes(1024), 1024);
        EXPECT_EQ(window.requestBytes(16 * 1024), 16 * 1024);
    }
}
