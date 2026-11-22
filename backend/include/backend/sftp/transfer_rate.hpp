#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

/**
 * @brief The rate of a transfer, taken from samples of its cumulative byte count and smoothed so the shown number
 *        stays readable.
 */
class TransferRate
{
  public:
    /** @brief The least time between two samples, the rate stays as it is in between. */
    static constexpr auto sampleInterval = std::chrono::milliseconds{1000};
    /** @brief The weight of a new sample in the smoothed rate. */
    static constexpr double smoothing = 0.3;

    /**
     * @brief Takes @p bytes as the bytes transferred until @p now.
     * @return The rate in bytes per second.
     */
    std::int64_t sample(std::uint64_t bytes, std::chrono::steady_clock::time_point now);

    /**
     * @brief Lets the next sample start anew, so time in which nothing was meant to move counts for no rate, such as
     *        a pause. The rate stays as it is.
     */
    void restart();

    std::int64_t bytesPerSecond() const;

  private:
    std::optional<std::chrono::steady_clock::time_point> lastSampleTime_{};
    std::uint64_t lastSampleBytes_{0};
    std::int64_t bytesPerSecond_{0};
};
