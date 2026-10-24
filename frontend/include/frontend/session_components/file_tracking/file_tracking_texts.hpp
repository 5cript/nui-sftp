#pragma once

#include <shared_data/file_tracking/upload_failure.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>
#include <paths/paths.hpp>

#include <cstdint>
#include <string>

namespace FileTracking
{
    /**
     * @brief Translated texts the pure parts format. Placeholders are fmt "{}" fields, filled in the order given on
     *        each field. translatedTexts() in translated_texts.hpp fills them from the language files; tests fill them
     *        by hand.
     */
    struct FileTrackingTexts
    {
        /**
         * @brief The counts beside a watch in the outliner, also used by its status badge.
         */
        struct Counts
        {
            /** @brief "1 conflict". */
            std::string conflictOne{};
            /** @brief "{} conflicts" (count). */
            std::string conflicts{};
            /** @brief "{} failed" (count). */
            std::string failed{};
        };

        /**
         * @brief The short reason beside a stopped watch in the outliner.
         */
        struct StopSides
        {
            /** @brief "stopped". */
            std::string stoppedByUser{};
            /** @brief "overlap". */
            std::string overlap{};
            /** @brief "deleted". */
            std::string copyGone{};
            /** @brief "connection lost". */
            std::string connectionLost{};
        };

        /**
         * @brief The status badge texts that are not counts.
         */
        struct Statuses
        {
            /** @brief "Syncing". */
            std::string syncing{};
            /** @brief "Up to date". */
            std::string upToDate{};
            /** @brief "Stopped". */
            std::string stopped{};
        };

        /**
         * @brief The sentence of the stop reason strip.
         */
        struct StopReasons
        {
            /** @brief "You stopped the watch at {}." (clock time). */
            std::string stoppedByUser{};
            /** @brief "Moved into {}, which the watch “{}” covers." (moved-to path, other watch's name). */
            std::string overlap{};
            /** @brief "Moved onto the server path of another watch.", an overlap that lacks the details. */
            std::string overlapUnknown{};
            /** @brief "The copy was deleted or renamed. The server's file is unchanged." */
            std::string copyGone{};
            /** @brief "The connection was lost at {}, the watch stopped." (clock time). */
            std::string connectionLost{};
        };

        /**
         * @brief One sentence per SharedData::UploadFailureReason.
         */
        struct FailureReasons
        {
            std::string lockedLocally{};
            std::string permissionDeniedLocally{};
            std::string unreadableLocally{};
            std::string permissionDeniedOnServer{};
            std::string noSpaceOnServer{};
            std::string serverRefused{};
            std::string connectionProblem{};
            std::string notQueued{};
            std::string unknown{};
        };

        /**
         * @brief The line under a conflicting file.
         */
        struct ConflictLines
        {
            /** @brief "Changed on the server at {}, saved here at {}" (server time, local time). */
            std::string changedOnServer{};
            /** @brief "Deleted on the server, saved here at {}" (local time). */
            std::string deletedOnServer{};
            /** @brief "Created on the server at {}, saved here at {}" (server time, local time). */
            std::string createdOnServer{};
            /** @brief "--:--", stands in for a time that is not known. */
            std::string timeUnknown{};
        };

        Counts counts{};
        StopSides stopSides{};
        Statuses statuses{};
        StopReasons stopReasons{};
        FailureReasons failureReasons{};
        ConflictLines conflictLines{};
    };

    /**
     * @brief "1 conflict" or "{} conflicts" for @p count.
     */
    std::string conflictCountText(FileTrackingTexts const& texts, std::uint64_t count);

    /**
     * @brief "{} failed" for @p count.
     */
    std::string failedCountText(FileTrackingTexts const& texts, std::uint64_t count);

    /**
     * @brief The short outliner reason of a watch that stopped for @p reason.
     */
    std::string stopSideText(FileTrackingTexts const& texts, SharedData::WatchStopReason reason);

    /**
     * @brief The sentence of the stop reason strip for @p stop. Overlap uses @ref
     * FileTrackingTexts::StopReasons::overlap when movedTo and otherWatchName are set, overlapUnknown otherwise.
     */
    std::string stopReasonText(FileTrackingTexts const& texts, SharedData::WatchStop const& stop);

    /**
     * @brief The sentence for @p reason.
     */
    std::string failureReasonText(FileTrackingTexts const& texts, SharedData::UploadFailureReason reason);

    /**
     * @brief The line under a conflicting file, with its times as local clock times, timeUnknown for a missing one.
     */
    std::string conflictLineText(FileTrackingTexts const& texts, SharedData::WatchConflict const& conflict);

    /**
     * @brief "HH:MM" of @p secondsSinceEpoch, a UTC time, shifted by @p utcOffsetSeconds.
     */
    std::string clockTime(std::uint64_t secondsSinceEpoch, std::int64_t utcOffsetSeconds);

    /**
     * @brief The offset of the local time zone from UTC at @p secondsSinceEpoch, in seconds.
     */
    std::int64_t localUtcOffset(std::uint64_t secondsSinceEpoch);

    /**
     * @brief "HH:MM" of @p secondsSinceEpoch in local time.
     */
    std::string localClockTime(std::uint64_t secondsSinceEpoch);

    /**
     * @brief "YYYY-MM-DD HH:MM" of @p secondsSinceEpoch in local time.
     */
    std::string localDateTime(std::uint64_t secondsSinceEpoch);

    /**
     * @brief @p bytes with its unit, as Utility::formatBytes writes it.
     */
    std::string sizeText(std::uint64_t bytes);

    /**
     * @brief How a file of a watch is shown in its lists: its path below @p watched with '/' separators, or its file
     * name when @p path is @p watched itself or not below it.
     */
    std::string pathWithin(Paths::LocalPath const& watched, Paths::LocalPath const& path);
}
