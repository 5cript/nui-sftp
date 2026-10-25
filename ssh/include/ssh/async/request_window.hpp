#pragma once

#include <ssh/async/transfer_chunk_controller.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace SecureShell
{
    /**
     * @brief Sizes the requests a pipelined transfer keeps in flight from the rate it achieves.
     *
     * The window holds about one target call duration of data at the measured rate, so draining
     * it blocks the processing thread no longer than one blocking call would. That duration spans
     * several round trips, so a window that the latency limits grows with every sample until the
     * link is the limit. A request is a fraction of the window, so waiting for the oldest one
     * takes a fraction of the target duration once the link is full.
     */
    class RequestWindow
    {
      public:
        static constexpr std::int64_t minimumWindow = 64 * 1024;
        static constexpr std::int64_t maximumWindow = 16 * 1024 * 1024;
        static constexpr std::int64_t initialWindow = 256 * 1024;
        static constexpr std::int64_t requestsPerWindow = 8;

        /**
         * @brief Feeds bytes that arrived, sampling the rate every half target duration.
         *
         * @param bytes Bytes one completed request moved.
         * @param now When the request completed.
         * @param target The session's target call duration.
         */
        void recordReceived(
            std::int64_t bytes,
            std::chrono::steady_clock::time_point now,
            std::chrono::steady_clock::duration target
        ) noexcept
        {
            if (bytes <= 0)
                return;
            if (sampleStart_ == std::chrono::steady_clock::time_point{})
                sampleStart_ = now;
            sampleBytes_ += bytes;

            const auto elapsed = now - sampleStart_;
            if (elapsed < target / 2)
                return;

            const auto elapsedNanos =
                std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            const auto targetNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(target).count();
            const auto ideal = std::min(maximumWindow, sampleBytes_ * targetNanos / elapsedNanos);
            windowBytes_ = std::clamp((windowBytes_ + ideal + 1) / 2, minimumWindow, maximumWindow);
            sampleStart_ = now;
            sampleBytes_ = 0;
        }

        /**
         * @brief Starts a new rate sample, so time in which nothing was requested (a pause) does
         *        not count as a slow link.
         */
        void restartSample() noexcept
        {
            sampleStart_ = {};
            sampleBytes_ = 0;
        }

        /**
         * @brief Bytes to keep requested but not yet received.
         */
        std::int64_t windowBytes() const noexcept
        {
            return windowBytes_;
        }

        /**
         * @brief Bytes one request should ask for.
         *
         * @param upperBound Buffer size or server limit the result must not exceed.
         */
        std::int64_t requestBytes(std::int64_t upperBound) const noexcept
        {
            const auto lowerBound = std::min(TransferChunkController::minimumChunk, upperBound);
            return std::clamp(windowBytes_ / requestsPerWindow, lowerBound, std::max(lowerBound, upperBound));
        }

      private:
        std::int64_t windowBytes_{initialWindow};
        std::chrono::steady_clock::time_point sampleStart_{};
        std::int64_t sampleBytes_{0};
    };
}
