#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <utility/format_bytes.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string>

namespace FileTracking
{
    namespace
    {
        constexpr std::int64_t secondsPerMinute = 60;
        constexpr std::int64_t secondsPerHour = 3'600;
        constexpr std::int64_t secondsPerDay = 86'400;

        /**
         * @brief A day of the proleptic Gregorian calendar.
         */
        struct CivilDate
        {
            std::int64_t year{1970};
            std::int64_t month{1};
            std::int64_t day{1};
        };

        /**
         * @brief @p value / @p divisor rounded towards negative infinity, @p divisor is positive.
         */
        constexpr std::int64_t floorDivide(std::int64_t value, std::int64_t divisor)
        {
            const auto quotient = value / divisor;
            return value % divisor != 0 && value < 0 ? quotient - 1 : quotient;
        }

        /**
         * @brief Days since 1970-01-01 of @p date (Howard Hinnant's days_from_civil).
         */
        constexpr std::int64_t daysFromCivil(CivilDate const& date)
        {
            const auto year = date.month <= 2 ? date.year - 1 : date.year;
            const auto era = floorDivide(year, 400);
            const auto yearOfEra = year - era * 400;
            const auto dayOfYear = (153 * (date.month > 2 ? date.month - 3 : date.month + 9) + 2) / 5 + date.day - 1;
            const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
            return era * 146'097 + dayOfEra - 719'468;
        }

        /**
         * @brief The date @p days after 1970-01-01 (Howard Hinnant's civil_from_days).
         */
        constexpr CivilDate civilFromDays(std::int64_t days)
        {
            const auto shifted = days + 719'468;
            const auto era = floorDivide(shifted, 146'097);
            const auto dayOfEra = shifted - era * 146'097;
            const auto yearOfEra = (dayOfEra - dayOfEra / 1'460 + dayOfEra / 36'524 - dayOfEra / 146'096) / 365;
            const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
            const auto monthIndex = (5 * dayOfYear + 2) / 153;
            const auto month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
            return CivilDate{
                .year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0),
                .month = month,
                .day = dayOfYear - (153 * monthIndex + 2) / 5 + 1,
            };
        }

        static_assert(daysFromCivil(CivilDate{.year = 1970, .month = 1, .day = 1}) == 0);
        static_assert(daysFromCivil(CivilDate{.year = 2000, .month = 3, .day = 1}) == 11'017);
        static_assert(civilFromDays(-1).year == 1969 && civilFromDays(-1).month == 12 && civilFromDays(-1).day == 31);
        static_assert(civilFromDays(11'016).month == 2 && civilFromDays(11'016).day == 29);

        /**
         * @brief @p secondsSinceEpoch shifted by @p utcOffsetSeconds.
         */
        std::int64_t shiftedSeconds(std::uint64_t secondsSinceEpoch, std::int64_t utcOffsetSeconds)
        {
            const auto limited = std::min<std::uint64_t>(
                secondsSinceEpoch, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2)
            );
            return static_cast<std::int64_t>(limited) + utcOffsetSeconds;
        }

        /**
         * @brief "HH:MM" of the second @p secondOfDay of a day.
         */
        std::string hoursAndMinutes(std::int64_t secondOfDay)
        {
            return fmt::format(
                "{:02}:{:02}", secondOfDay / secondsPerHour, secondOfDay % secondsPerHour / secondsPerMinute
            );
        }

        /**
         * @brief The local calendar time of @p time, nothing when the C library cannot tell.
         */
        std::optional<std::tm> localTimeOf(std::time_t time)
        {
            std::tm local{};
#ifdef _WIN32
            if (localtime_s(&local, &time) != 0)
                return std::nullopt;
#else
            if (localtime_r(&time, &local) == nullptr)
                return std::nullopt;
#endif
            return local;
        }

        /**
         * @brief @p time as local clock time, or @p unknownText when there is none.
         */
        std::string clockTimeOr(std::optional<std::uint64_t> const& time, std::string const& unknownText)
        {
            return time.has_value() ? localClockTime(*time) : unknownText;
        }
    }

    std::string conflictCountText(FileTrackingTexts const& texts, std::uint64_t count)
    {
        if (count == 1u)
            return texts.counts.conflictOne;
        return fmt::format(fmt::runtime(texts.counts.conflicts), count);
    }

    std::string failedCountText(FileTrackingTexts const& texts, std::uint64_t count)
    {
        return fmt::format(fmt::runtime(texts.counts.failed), count);
    }

    std::string stopSideText(FileTrackingTexts const& texts, SharedData::WatchStopReason reason)
    {
        switch (reason)
        {
            case SharedData::WatchStopReason::StoppedByUser:
                return texts.stopSides.stoppedByUser;
            case SharedData::WatchStopReason::Overlap:
                return texts.stopSides.overlap;
            case SharedData::WatchStopReason::CopyGone:
                return texts.stopSides.copyGone;
            case SharedData::WatchStopReason::ConnectionLost:
                return texts.stopSides.connectionLost;
        }
        return texts.stopSides.stoppedByUser;
    }

    std::string stopReasonText(FileTrackingTexts const& texts, SharedData::WatchStop const& stop)
    {
        switch (stop.reason)
        {
            case SharedData::WatchStopReason::StoppedByUser:
                return fmt::format(fmt::runtime(texts.stopReasons.stoppedByUser), localClockTime(stop.stoppedAt));
            case SharedData::WatchStopReason::Overlap:
                if (stop.movedTo.has_value() && stop.otherWatchName.has_value())
                    return fmt::format(
                        fmt::runtime(texts.stopReasons.overlap), stop.movedTo->toUtf8(), *stop.otherWatchName
                    );
                return texts.stopReasons.overlapUnknown;
            case SharedData::WatchStopReason::CopyGone:
                return texts.stopReasons.copyGone;
            case SharedData::WatchStopReason::ConnectionLost:
                return fmt::format(fmt::runtime(texts.stopReasons.connectionLost), localClockTime(stop.stoppedAt));
        }
        return fmt::format(fmt::runtime(texts.stopReasons.stoppedByUser), localClockTime(stop.stoppedAt));
    }

    std::string failureReasonText(FileTrackingTexts const& texts, SharedData::UploadFailureReason reason)
    {
        auto const& reasons = texts.failureReasons;
        switch (reason)
        {
            case SharedData::UploadFailureReason::LockedLocally:
                return reasons.lockedLocally;
            case SharedData::UploadFailureReason::PermissionDeniedLocally:
                return reasons.permissionDeniedLocally;
            case SharedData::UploadFailureReason::UnreadableLocally:
                return reasons.unreadableLocally;
            case SharedData::UploadFailureReason::PermissionDeniedOnServer:
                return reasons.permissionDeniedOnServer;
            case SharedData::UploadFailureReason::NoSpaceOnServer:
                return reasons.noSpaceOnServer;
            case SharedData::UploadFailureReason::ServerRefused:
                return reasons.serverRefused;
            case SharedData::UploadFailureReason::ConnectionProblem:
                return reasons.connectionProblem;
            case SharedData::UploadFailureReason::NotQueued:
                return reasons.notQueued;
            case SharedData::UploadFailureReason::Unknown:
                return reasons.unknown;
        }
        return reasons.unknown;
    }

    std::string conflictLineText(FileTrackingTexts const& texts, SharedData::WatchConflict const& conflict)
    {
        auto const& lines = texts.conflictLines;
        const auto serverTime = clockTimeOr(conflict.serverModifiedAt, lines.timeUnknown);
        const auto localTime = clockTimeOr(conflict.localModifiedAt, lines.timeUnknown);
        switch (conflict.kind)
        {
            case SharedData::ConflictKind::ChangedOnServer:
                return fmt::format(fmt::runtime(lines.changedOnServer), serverTime, localTime);
            case SharedData::ConflictKind::DeletedOnServer:
                return fmt::format(fmt::runtime(lines.deletedOnServer), localTime);
            case SharedData::ConflictKind::CreatedOnServer:
                return fmt::format(fmt::runtime(lines.createdOnServer), serverTime, localTime);
        }
        return fmt::format(fmt::runtime(lines.changedOnServer), serverTime, localTime);
    }

    std::string clockTime(std::uint64_t secondsSinceEpoch, std::int64_t utcOffsetSeconds)
    {
        const auto local = shiftedSeconds(secondsSinceEpoch, utcOffsetSeconds);
        return hoursAndMinutes(local - floorDivide(local, secondsPerDay) * secondsPerDay);
    }

    std::string dateTime(std::uint64_t secondsSinceEpoch, std::int64_t utcOffsetSeconds)
    {
        const auto local = shiftedSeconds(secondsSinceEpoch, utcOffsetSeconds);
        const auto days = floorDivide(local, secondsPerDay);
        const auto date = civilFromDays(days);
        return fmt::format(
            "{:04}-{:02}-{:02} {}", date.year, date.month, date.day, hoursAndMinutes(local - days * secondsPerDay)
        );
    }

    std::int64_t localUtcOffset(std::uint64_t secondsSinceEpoch)
    {
        const auto time = static_cast<std::time_t>(shiftedSeconds(secondsSinceEpoch, 0));
        const auto local = localTimeOf(time);
        if (!local.has_value())
            return 0;
        const auto days = daysFromCivil(
            CivilDate{
                .year = std::int64_t{local->tm_year} + 1'900,
                .month = std::int64_t{local->tm_mon} + 1,
                .day = std::int64_t{local->tm_mday},
            }
        );
        const auto localSeconds = days * secondsPerDay + std::int64_t{local->tm_hour} * secondsPerHour +
            std::int64_t{local->tm_min} * secondsPerMinute + std::int64_t{local->tm_sec};
        return localSeconds - static_cast<std::int64_t>(time);
    }

    std::string localClockTime(std::uint64_t secondsSinceEpoch)
    {
        return clockTime(secondsSinceEpoch, localUtcOffset(secondsSinceEpoch));
    }

    std::string localDateTime(std::uint64_t secondsSinceEpoch)
    {
        return dateTime(secondsSinceEpoch, localUtcOffset(secondsSinceEpoch));
    }

    std::string sizeText(std::uint64_t bytes)
    {
        const auto limited =
            std::min<std::uint64_t>(bytes, static_cast<std::uint64_t>(std::numeric_limits<long long>::max()));
        return Utility::formatBytes(static_cast<long long>(limited));
    }

    std::string pathWithin(Paths::LocalPath const& watched, Paths::LocalPath const& path)
    {
        const auto relative = path.relativeTo(watched);
        if (relative.has_value() && !relative->empty())
            return relative->value();
        const auto name = path.filename();
        return name.has_value() ? name->value() : path.toUtf8();
    }
}
