#include <backend/sftp/transfer_rate.hpp>

std::int64_t TransferRate::sample(std::uint64_t bytes, std::chrono::steady_clock::time_point now)
{
    if (!lastSampleTime_)
    {
        lastSampleTime_ = now;
        lastSampleBytes_ = bytes;
        return bytesPerSecond_;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastSampleTime_);
    if (duration < sampleInterval)
        return bytesPerSecond_;
    const std::uint64_t moved = bytes >= lastSampleBytes_ ? bytes - lastSampleBytes_ : 0u;
    const double sampled = static_cast<double>(moved) * 1000.0 / static_cast<double>(duration.count());
    // The first sample is taken as it is, a rate that climbs up from zero would read as a slow start.
    const double smoothed = bytesPerSecond_ == 0
        ? sampled
        : smoothing * sampled + (1.0 - smoothing) * static_cast<double>(bytesPerSecond_);
    bytesPerSecond_ = static_cast<std::int64_t>(smoothed);
    lastSampleTime_ = now;
    lastSampleBytes_ = bytes;
    return bytesPerSecond_;
}

void TransferRate::restart()
{
    lastSampleTime_.reset();
}

std::int64_t TransferRate::bytesPerSecond() const
{
    return bytesPerSecond_;
}
