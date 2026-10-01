#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace SecureShell
{
    /**
     * @brief Sizes blocking sftp data calls so each takes about a target duration on the link.
     *
     * The round trip is learned from metadata calls (open, stat), which are pure round trips, and
     * data calls may only lower it: a data call never completes faster than one round trip, but it
     * always takes longer, so letting it raise the estimate would feed the controller its own output.
     * The chunk is derived from the payload portion of a call (duration minus round trip), so a link
     * whose round trip alone exceeds the target cannot shrink the chunk toward zero.
     */
    class TransferChunkController
    {
      public:
        static constexpr std::int64_t minimumChunk = 4 * 1024;
        static constexpr std::int64_t maximumChunk = 4 * 1024 * 1024;
        static constexpr std::int64_t initialChunk = 16 * 1024;

        /**
         * @brief Feeds the duration of one metadata call (open, stat, lstat), which approximates a
         *        round trip. Followed in both directions so a changing link is tracked.
         */
        void recordRoundTrip(std::chrono::steady_clock::duration took) noexcept
        {
            const auto tookNanos = nanos(took);
            const auto roundTrip = roundTripNanos_.load();
            if (roundTrip == 0)
                roundTripNanos_ = tookNanos;
            else
                roundTripNanos_ = roundTrip + (tookNanos - roundTrip) / 8;
        }

        /**
         * @brief Feeds the duration of one blocking data call (sftp_read / sftp_write).
         *
         * @param bytes Bytes the call moved.
         * @param requested Chunk the controller proposed for the call; a call that moved less (file
         *        tail, short read) does not size the chunk.
         * @param took Wall time the call blocked.
         */
        void recordDataCall(std::int64_t bytes, std::int64_t requested, std::chrono::steady_clock::duration took) noexcept
        {
            if (bytes <= 0)
                return;
            const auto tookNanos = nanos(took);

            const auto roundTrip = roundTripNanos_.load();
            if (roundTrip == 0 || tookNanos <= roundTrip)
            {
                // The call is the fastest round trip seen, so its payload portion is not
                // measurable and must not size the chunk.
                roundTripNanos_ = tookNanos;
                return;
            }

            if (bytes < requested)
                return;

            // Size from the payload portion only: the round trip is paid regardless of chunk size.
            const auto targetNanos = nanos(targetCallDuration());
            const auto payloadBudget = std::max(targetNanos - roundTrip, minimumPayloadNanos);
            const auto payloadTook = std::max<std::int64_t>(tookNanos - roundTrip, 1);
            const auto ideal = std::min(maximumChunk, bytes * payloadBudget / payloadTook);
            const auto current = chunkBytes_.load();
            chunkBytes_ = std::clamp((current + ideal) / 2, minimumChunk, maximumChunk);
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

        /**
         * @brief Bytes a single sftp read or write should move on this link right now,
         *        pessimistic (16 KiB) until calls have been measured.
         *
         * @param upperBound Buffer size or server limit the result must not exceed.
         */
        std::int64_t preferredTransferChunk(std::int64_t upperBound) const noexcept
        {
            return std::clamp(chunkBytes_.load(), minimumChunk, std::max(minimumChunk, upperBound));
        }

        std::int64_t roundTripNanos() const noexcept
        {
            return roundTripNanos_.load();
        }

      private:
        static std::int64_t nanos(std::chrono::steady_clock::duration duration) noexcept
        {
            return std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        }

      private:
        // Payload time a call keeps even when the round trip alone exceeds the target.
        static constexpr std::int64_t minimumPayloadNanos = 50'000'000;
        std::atomic<std::int64_t> chunkBytes_{initialChunk};
        std::atomic<std::int64_t> roundTripNanos_{0};
    };
}
