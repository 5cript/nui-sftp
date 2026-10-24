#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>

namespace FileTracking
{
    std::string conflictCountText([[maybe_unused]] FileTrackingTexts const& texts, [[maybe_unused]] std::uint64_t count)
    {
        return {};
    }

    std::string failedCountText([[maybe_unused]] FileTrackingTexts const& texts, [[maybe_unused]] std::uint64_t count)
    {
        return {};
    }

    std::string
    stopSideText([[maybe_unused]] FileTrackingTexts const& texts, [[maybe_unused]] SharedData::WatchStopReason reason)
    {
        return {};
    }

    std::string
    stopReasonText([[maybe_unused]] FileTrackingTexts const& texts, [[maybe_unused]] SharedData::WatchStop const& stop)
    {
        return {};
    }

    std::string failureReasonText(
        [[maybe_unused]] FileTrackingTexts const& texts,
        [[maybe_unused]] SharedData::UploadFailureReason reason
    )
    {
        return {};
    }

    std::string conflictLineText(
        [[maybe_unused]] FileTrackingTexts const& texts,
        [[maybe_unused]] SharedData::WatchConflict const& conflict
    )
    {
        return {};
    }

    std::string
    clockTime([[maybe_unused]] std::uint64_t secondsSinceEpoch, [[maybe_unused]] std::int64_t utcOffsetSeconds)
    {
        return {};
    }

    std::int64_t localUtcOffset([[maybe_unused]] std::uint64_t secondsSinceEpoch)
    {
        return 0;
    }

    std::string localClockTime([[maybe_unused]] std::uint64_t secondsSinceEpoch)
    {
        return {};
    }

    std::string localDateTime([[maybe_unused]] std::uint64_t secondsSinceEpoch)
    {
        return {};
    }

    std::string sizeText([[maybe_unused]] std::uint64_t bytes)
    {
        return {};
    }

    std::string
    pathWithin([[maybe_unused]] Paths::LocalPath const& watched, [[maybe_unused]] Paths::LocalPath const& path)
    {
        return {};
    }
}
