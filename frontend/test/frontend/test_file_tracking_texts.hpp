#pragma once

#include <frontend/session_components/file_tracking/file_tracking_texts.hpp>
#include <utility/format_bytes.hpp>
#include <paths/paths.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FileTracking::Test
{
    namespace FileTrackingTextsTestDetail
    {
        constexpr std::uint64_t serverTime = 1'700'000'000u;
        constexpr std::uint64_t localTime = 1'700'003'600u;

        /**
         * @brief Every text filled, as the English language file has them.
         */
        inline FileTrackingTexts sampleTexts()
        {
            FileTrackingTexts texts{};
            texts.counts = {.conflictOne = "1 conflict", .conflicts = "{} conflicts", .failed = "{} failed"};
            texts.stopSides = {
                .stoppedByUser = "stopped",
                .overlap = "overlap",
                .copyGone = "deleted",
                .connectionLost = "connection lost",
            };
            texts.statuses = {.syncing = "Syncing", .upToDate = "Up to date", .stopped = "Stopped"};
            texts.stopReasons = {
                .stoppedByUser = "You stopped the watch at {}.",
                .overlap = "Moved into {}, which the watch '{}' covers.",
                .overlapUnknown = "Moved onto the server path of another watch.",
                .copyGone = "The copy was deleted or renamed.",
                .connectionLost = "The connection was lost at {}, the watch stopped.",
            };
            texts.failureReasons = {
                .lockedLocally = "locked locally",
                .permissionDeniedLocally = "permission denied locally",
                .unreadableLocally = "unreadable locally",
                .permissionDeniedOnServer = "permission denied on the server",
                .noSpaceOnServer = "no space on the server",
                .serverRefused = "server refused",
                .connectionProblem = "connection problem",
                .notQueued = "not queued",
                .unknown = "unknown",
            };
            texts.conflictLines = {
                .changedOnServer = "Changed on the server at {}, saved here at {}",
                .deletedOnServer = "Deleted on the server, saved here at {}",
                .createdOnServer = "Created on the server at {}, saved here at {}",
                .timeUnknown = "--:--",
            };
            return texts;
        }

        inline Paths::LocalPath localPath(std::string_view path)
        {
            return *Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix);
        }

        inline SharedData::WatchStop stopOf(SharedData::WatchStopReason reason)
        {
            return SharedData::WatchStop{.reason = reason, .stoppedAt = serverTime};
        }

        inline SharedData::WatchConflict conflictOf(
            SharedData::ConflictKind kind,
            std::optional<std::uint64_t> serverModifiedAt,
            std::optional<std::uint64_t> localModifiedAt
        )
        {
            SharedData::WatchConflict conflict{};
            conflict.conflictId = Ids::makeConflictId("conflict");
            conflict.watchId = Ids::makeWatchId("watch");
            conflict.kind = kind;
            conflict.localPath = localPath("/home/user/notes.txt");
            conflict.remotePath = *Paths::RemotePath::make("/srv/notes.txt");
            conflict.serverModifiedAt = serverModifiedAt;
            conflict.localModifiedAt = localModifiedAt;
            conflict.raisedAt = localTime;
            return conflict;
        }
    }

    TEST(FileTrackingTextsTests, ConflictCountsUseTheSingularForOne)
    {
        const auto texts = FileTrackingTextsTestDetail::sampleTexts();

        EXPECT_EQ(conflictCountText(texts, 0u), "0 conflicts");
        EXPECT_EQ(conflictCountText(texts, 1u), "1 conflict");
        EXPECT_EQ(conflictCountText(texts, 2u), "2 conflicts");
        EXPECT_EQ(conflictCountText(texts, 1'234u), "1234 conflicts");
    }

    TEST(FileTrackingTextsTests, FailedCountsUseOneTextForEveryCount)
    {
        const auto texts = FileTrackingTextsTestDetail::sampleTexts();

        EXPECT_EQ(failedCountText(texts, 1u), "1 failed");
        EXPECT_EQ(failedCountText(texts, 5u), "5 failed");
    }

    TEST(FileTrackingTextsTests, EveryStopReasonHasItsSideText)
    {
        const auto texts = FileTrackingTextsTestDetail::sampleTexts();

        EXPECT_EQ(stopSideText(texts, SharedData::WatchStopReason::StoppedByUser), "stopped");
        EXPECT_EQ(stopSideText(texts, SharedData::WatchStopReason::Overlap), "overlap");
        EXPECT_EQ(stopSideText(texts, SharedData::WatchStopReason::CopyGone), "deleted");
        EXPECT_EQ(stopSideText(texts, SharedData::WatchStopReason::ConnectionLost), "connection lost");
    }

    TEST(FileTrackingTextsTests, AStopByTheUserTellsItsLocalTime)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto texts = sampleTexts();

        EXPECT_EQ(
            stopReasonText(texts, stopOf(SharedData::WatchStopReason::StoppedByUser)),
            fmt::format("You stopped the watch at {}.", localClockTime(serverTime))
        );
    }

    TEST(FileTrackingTextsTests, ALostConnectionTellsItsLocalTime)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto texts = sampleTexts();

        EXPECT_EQ(
            stopReasonText(texts, stopOf(SharedData::WatchStopReason::ConnectionLost)),
            fmt::format("The connection was lost at {}, the watch stopped.", localClockTime(serverTime))
        );
    }

    TEST(FileTrackingTextsTests, AGoneCopyNeedsNoDetails)
    {
        using namespace FileTrackingTextsTestDetail;

        EXPECT_EQ(
            stopReasonText(sampleTexts(), stopOf(SharedData::WatchStopReason::CopyGone)),
            "The copy was deleted or renamed."
        );
    }

    TEST(FileTrackingTextsTests, AnOverlapNamesWhereTheItemMovedAndTheOtherWatch)
    {
        using namespace FileTrackingTextsTestDetail;
        auto stop = stopOf(SharedData::WatchStopReason::Overlap);
        stop.otherWatchId = Ids::makeWatchId("other");
        stop.otherWatchName = "project";
        stop.movedTo = localPath("/home/user/project/notes.txt");

        EXPECT_EQ(
            stopReasonText(sampleTexts(), stop),
            "Moved into /home/user/project/notes.txt, which the watch 'project' covers."
        );
    }

    TEST(FileTrackingTextsTests, AnOverlapWithoutItsDetailsFallsBackToTheGeneralSentence)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto texts = sampleTexts();
        const auto fallback = std::string{"Moved onto the server path of another watch."};

        auto withoutAnything = stopOf(SharedData::WatchStopReason::Overlap);
        withoutAnything.otherWatchId = Ids::makeWatchId("other");
        EXPECT_EQ(stopReasonText(texts, withoutAnything), fallback);

        auto withoutName = stopOf(SharedData::WatchStopReason::Overlap);
        withoutName.movedTo = localPath("/home/user/project/notes.txt");
        EXPECT_EQ(stopReasonText(texts, withoutName), fallback);

        auto withoutTarget = stopOf(SharedData::WatchStopReason::Overlap);
        withoutTarget.otherWatchName = "project";
        EXPECT_EQ(stopReasonText(texts, withoutTarget), fallback);
    }

    TEST(FileTrackingTextsTests, EveryFailureReasonHasItsSentence)
    {
        const auto texts = FileTrackingTextsTestDetail::sampleTexts();
        const std::vector<std::pair<SharedData::UploadFailureReason, std::string>> expected{
            {SharedData::UploadFailureReason::LockedLocally, "locked locally"},
            {SharedData::UploadFailureReason::PermissionDeniedLocally, "permission denied locally"},
            {SharedData::UploadFailureReason::UnreadableLocally, "unreadable locally"},
            {SharedData::UploadFailureReason::PermissionDeniedOnServer, "permission denied on the server"},
            {SharedData::UploadFailureReason::NoSpaceOnServer, "no space on the server"},
            {SharedData::UploadFailureReason::ServerRefused, "server refused"},
            {SharedData::UploadFailureReason::ConnectionProblem, "connection problem"},
            {SharedData::UploadFailureReason::NotQueued, "not queued"},
            {SharedData::UploadFailureReason::Unknown, "unknown"},
        };

        for (auto const& [reason, sentence] : expected)
            EXPECT_EQ(failureReasonText(texts, reason), sentence);
    }

    TEST(FileTrackingTextsTests, AServerChangeTellsBothLocalTimes)
    {
        using namespace FileTrackingTextsTestDetail;

        EXPECT_EQ(
            conflictLineText(
                sampleTexts(), conflictOf(SharedData::ConflictKind::ChangedOnServer, serverTime, localTime)
            ),
            fmt::format(
                "Changed on the server at {}, saved here at {}", localClockTime(serverTime), localClockTime(localTime)
            )
        );
    }

    TEST(FileTrackingTextsTests, AServerDeletionTellsTheLocalSave)
    {
        using namespace FileTrackingTextsTestDetail;

        EXPECT_EQ(
            conflictLineText(
                sampleTexts(), conflictOf(SharedData::ConflictKind::DeletedOnServer, std::nullopt, localTime)
            ),
            fmt::format("Deleted on the server, saved here at {}", localClockTime(localTime))
        );
    }

    TEST(FileTrackingTextsTests, AServerCreationTellsBothLocalTimes)
    {
        using namespace FileTrackingTextsTestDetail;

        EXPECT_EQ(
            conflictLineText(
                sampleTexts(), conflictOf(SharedData::ConflictKind::CreatedOnServer, serverTime, localTime)
            ),
            fmt::format(
                "Created on the server at {}, saved here at {}", localClockTime(serverTime), localClockTime(localTime)
            )
        );
    }

    TEST(FileTrackingTextsTests, UnknownTimesShowThePlaceholder)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto texts = sampleTexts();

        EXPECT_EQ(
            conflictLineText(texts, conflictOf(SharedData::ConflictKind::ChangedOnServer, std::nullopt, std::nullopt)),
            "Changed on the server at --:--, saved here at --:--"
        );
        EXPECT_EQ(
            conflictLineText(texts, conflictOf(SharedData::ConflictKind::ChangedOnServer, serverTime, std::nullopt)),
            fmt::format("Changed on the server at {}, saved here at --:--", localClockTime(serverTime))
        );
        EXPECT_EQ(
            conflictLineText(texts, conflictOf(SharedData::ConflictKind::DeletedOnServer, serverTime, std::nullopt)),
            "Deleted on the server, saved here at --:--"
        );
        EXPECT_EQ(
            conflictLineText(texts, conflictOf(SharedData::ConflictKind::CreatedOnServer, std::nullopt, localTime)),
            fmt::format("Created on the server at --:--, saved here at {}", localClockTime(localTime))
        );
    }

    TEST(FileTrackingTextsTests, ClockTimeInUtc)
    {
        EXPECT_EQ(clockTime(0u, 0), "00:00");
        EXPECT_EQ(clockTime(59u, 0), "00:00");
        EXPECT_EQ(clockTime(60u, 0), "00:01");
        EXPECT_EQ(clockTime(1'700'000'000u, 0), "22:13");
    }

    TEST(FileTrackingTextsTests, ClockTimeWithAPositiveOffset)
    {
        EXPECT_EQ(clockTime(1'700'000'000u, 3'600), "23:13");
        EXPECT_EQ(clockTime(1'700'000'000u, 7'200), "00:13");
        EXPECT_EQ(clockTime(1'700'000'000u, 19'800), "03:43");
        EXPECT_EQ(clockTime(1'700'000'000u, 20'700), "03:58");
    }

    TEST(FileTrackingTextsTests, ClockTimeWithANegativeOffset)
    {
        EXPECT_EQ(clockTime(1'700'000'000u, -18'000), "17:13");
        EXPECT_EQ(clockTime(1'700'000'000u, -80'000), "00:00");
        EXPECT_EQ(clockTime(1'700'000'000u, -80'001), "23:59");
        EXPECT_EQ(clockTime(1'699'920'600u, -3'600), "23:10");
    }

    TEST(FileTrackingTextsTests, ClockTimeAroundMidnight)
    {
        EXPECT_EQ(clockTime(86'399u, 0), "23:59");
        EXPECT_EQ(clockTime(86'400u, 0), "00:00");
        EXPECT_EQ(clockTime(86'340u, 59), "23:59");
        EXPECT_EQ(clockTime(86'340u, 60), "00:00");
        EXPECT_EQ(clockTime(0u, -3'600), "23:00");
        EXPECT_EQ(clockTime(59u, -60), "23:59");
    }

    TEST(FileTrackingTextsTests, DateTimeWithFixedOffsets)
    {
        EXPECT_EQ(dateTime(0u, 0), "1970-01-01 00:00");
        EXPECT_EQ(dateTime(0u, -3'600), "1969-12-31 23:00");
        EXPECT_EQ(dateTime(1'700'000'000u, 0), "2023-11-14 22:13");
        EXPECT_EQ(dateTime(1'700'000'000u, 7'200), "2023-11-15 00:13");
        EXPECT_EQ(dateTime(1'700'000'000u, -80'001), "2023-11-13 23:59");
        EXPECT_EQ(dateTime(951'827'400u, 0), "2000-02-29 12:30");
        EXPECT_EQ(dateTime(951'827'400u, 43'200), "2000-03-01 00:30");
        EXPECT_EQ(dateTime(1'735'689'540u, 0), "2024-12-31 23:59");
        EXPECT_EQ(dateTime(1'735'689'540u, 60), "2025-01-01 00:00");
    }

    TEST(FileTrackingTextsTests, LocalTimesUseTheLocalOffset)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto offset = localUtcOffset(serverTime);
        const auto day = std::int64_t{86'400};

        EXPECT_GT(offset, -day);
        EXPECT_LT(offset, day);
        EXPECT_EQ(localClockTime(serverTime), clockTime(serverTime, offset));
        EXPECT_EQ(localDateTime(serverTime), dateTime(serverTime, offset));
        EXPECT_EQ(localClockTime(serverTime).size(), std::size_t{5});
        EXPECT_EQ(localDateTime(serverTime).size(), std::size_t{16});
    }

    TEST(FileTrackingTextsTests, SizesAreWrittenAsFormatBytesWritesThem)
    {
        EXPECT_EQ(sizeText(0u), Utility::formatBytes(0));
        EXPECT_EQ(sizeText(512u), Utility::formatBytes(512));
        EXPECT_EQ(sizeText(1'536u), "1.50 KB");
        EXPECT_EQ(sizeText(5'242'880u), "5.00 MB");
        EXPECT_EQ(
            sizeText(std::numeric_limits<std::uint64_t>::max()),
            Utility::formatBytes(std::numeric_limits<long long>::max())
        );
    }

    TEST(FileTrackingTextsTests, APathBelowTheWatchedDirectoryIsShownRelative)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto watched = localPath("/home/user/project");

        EXPECT_EQ(pathWithin(watched, localPath("/home/user/project/notes.txt")), "notes.txt");
        EXPECT_EQ(pathWithin(watched, localPath("/home/user/project/source/main.cpp")), "source/main.cpp");
    }

    TEST(FileTrackingTextsTests, TheWatchedItemItselfIsShownByItsName)
    {
        using namespace FileTrackingTextsTestDetail;

        EXPECT_EQ(pathWithin(localPath("/home/user/notes.txt"), localPath("/home/user/notes.txt")), "notes.txt");
        EXPECT_EQ(pathWithin(localPath("/home/user/project"), localPath("/home/user/project")), "project");
    }

    TEST(FileTrackingTextsTests, APathOutsideTheWatchedItemIsShownByItsName)
    {
        using namespace FileTrackingTextsTestDetail;
        const auto watched = localPath("/home/user/project");

        EXPECT_EQ(pathWithin(watched, localPath("/home/user/other/notes.txt")), "notes.txt");
        EXPECT_EQ(pathWithin(watched, localPath("/home/user/project-old/notes.txt")), "notes.txt");
        EXPECT_EQ(pathWithin(watched, localPath("/")), "/");
    }
}
