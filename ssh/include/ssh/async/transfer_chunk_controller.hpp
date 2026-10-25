#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace SecureShell
{
    /**
     * @brief Learns the link's round trip from metadata calls (open, stat), which are pure round trips, and
     *        derives from it how long a turn of a transfer may block the processing thread.
     */
    class TransferChunkController
    {
      public:
        /** @brief The smallest request a transfer sends. */
        static constexpr std::int64_t minimumChunk = 4 * 1024;

        /**
         * @brief Feeds the duration of one metadata call (open, stat, lstat), which approximates a
         *        round trip. Followed in both directions so a changing link is tracked.
         */
        void recordRoundTrip(std::chrono::steady_clock::duration took) noexcept
        {
            const auto tookNanos = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::nanoseconds>(took).count()
            );
            const auto roundTrip = roundTripNanos_.load();
            if (roundTrip == 0)
                roundTripNanos_ = tookNanos;
            else
                roundTripNanos_ = roundTrip + (tookNanos - roundTrip) / 8;
        }

        /**
         * @brief How long one blocking data call should take: a few round trips so the call stays
         *        efficient, clamped to 50..250 ms so the processing thread stays responsive.
         */
        std::chrono::steady_clock::duration targetCallDuration() const noexcept
        {
            using namespace std::chrono_literals;
            constexpr auto minimum = 50ms;
            constexpr auto maximum = 250ms;
            constexpr std::int64_t roundTripsPerCall = 4;
            const auto roundTrip = std::chrono::nanoseconds{roundTripNanos_.load()};
            if (roundTrip.count() == 0)
                return minimum;
            return std::clamp<std::chrono::steady_clock::duration>(roundTrip * roundTripsPerCall, minimum, maximum);
        }

        std::int64_t roundTripNanos() const noexcept
        {
            return roundTripNanos_.load();
        }

      private:
        std::atomic<std::int64_t> roundTripNanos_{0};
    };
}
