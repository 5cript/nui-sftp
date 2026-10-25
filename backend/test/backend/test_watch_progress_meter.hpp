#pragma once

#include <backend/file_tracking/tracker_remote.hpp>
#include <backend/file_tracking/watch_progress_meter.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/watch_progress.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Test::WatchProgressMeterCases
{
    using UploadResult = FileTracking::WatchProgressMeter::UploadResult;

    inline Paths::LocalPath localPathOf(std::string_view path)
    {
        return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
    }

    inline SharedData::WatchFileProgress fileAt(std::string_view path, std::uint64_t bytes, std::uint64_t totalBytes)
    {
        return SharedData::WatchFileProgress{.path = localPathOf(path), .bytes = bytes, .totalBytes = totalBytes};
    }

    /**
     * @brief The progress of an upload of a single file that is on its way.
     */
    inline FileTracking::UploadProgress
    fileProgress(std::string_view path, std::uint64_t bytesSent, std::uint64_t bytesTotal, bool transferred = false)
    {
        return FileTracking::UploadProgress{
            .bytesSent = bytesSent,
            .bytesTotal = bytesTotal,
            .inFlight = {fileAt(path, bytesSent, bytesTotal)},
            .filesDone = 0u,
            .fileCount = 1u,
            .transferred = transferred,
        };
    }

    inline std::vector<std::string> pathsOf(std::vector<SharedData::WatchFileProgress> const& inFlight)
    {
        std::vector<std::string> paths;
        for (auto const& file : inFlight)
            paths.push_back(file.path.toUtf8());
        return paths;
    }

    class WatchProgressMeterTest : public ::testing::Test
    {
      protected:
        Ids::WatchId watch_{Ids::makeWatchId("watch-one")};
        FileTracking::WatchProgressMeter meter_{watch_};

        /**
         * @brief Ends the upload of @p path that stood for @p changes changes, @p pendingAfter are on their way then,
         *        like the tracker does it.
         */
        void endUpload(std::string_view path, UploadResult result, std::uint64_t changes, std::uint64_t pendingAfter)
        {
            meter_.uploadEnded(localPathOf(path), result);
            meter_.changesEnded(changes);
            meter_.setChangesPending(pendingAfter);
        }
    };

    TEST_F(WatchProgressMeterTest, ANewMeterShowsNothingOnItsWay)
    {
        const auto progress = meter_.snapshot();

        EXPECT_EQ(meter_.watchId(), watch_);
        EXPECT_EQ(progress.watchId, watch_);
        EXPECT_EQ(progress.changesPending, 0u);
        EXPECT_EQ(progress.changesDone, 0u);
        EXPECT_EQ(progress.changesWaiting, 0u);
        EXPECT_EQ(progress.waitingForAnswer, 0u);
        EXPECT_EQ(progress.bytesDone, 0u);
        EXPECT_EQ(progress.bytesTotal, 0u);
        EXPECT_TRUE(progress.inFlight.empty());
        EXPECT_FALSE(progress.lastSync.has_value());
        EXPECT_TRUE(meter_.isIdle());
        EXPECT_FALSE(meter_.isDirty());
    }

    TEST_F(WatchProgressMeterTest, AChangeWaitsUntilItsUploadStartsOnTheServer)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);

        const auto queued = meter_.snapshot();
        EXPECT_EQ(queued.changesPending, 1u);
        EXPECT_EQ(queued.changesWaiting, 1u);
        EXPECT_EQ(queued.changesDone, 0u);
        EXPECT_EQ(queued.bytesDone, 0u);
        EXPECT_EQ(queued.bytesTotal, 100u);
        EXPECT_TRUE(queued.inFlight.empty());
        EXPECT_FALSE(meter_.isIdle());

        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 40u, 100u));

        const auto running = meter_.snapshot();
        EXPECT_EQ(running.changesPending, 1u);
        EXPECT_EQ(running.changesWaiting, 0u);
        EXPECT_EQ(running.bytesDone, 40u);
        EXPECT_EQ(running.bytesTotal, 100u);
        ASSERT_EQ(running.inFlight.size(), 1u);
        EXPECT_EQ(running.inFlight.front().path.toUtf8(), "/watched/a.txt");
        EXPECT_EQ(running.inFlight.front().bytes, 40u);
        EXPECT_EQ(running.inFlight.front().totalBytes, 100u);
    }

    TEST_F(WatchProgressMeterTest, ACompletedUploadCountsAllItsBytesAsDone)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 40u, 100u));

        endUpload("/watched/a.txt", UploadResult::Completed, 1u, 0u);

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.changesPending, 0u);
        EXPECT_EQ(progress.changesDone, 1u);
        EXPECT_EQ(progress.changesWaiting, 0u);
        EXPECT_EQ(progress.bytesDone, 100u);
        EXPECT_EQ(progress.bytesTotal, 100u);
        EXPECT_TRUE(progress.inFlight.empty());
        EXPECT_TRUE(meter_.isIdle());
    }

    TEST_F(WatchProgressMeterTest, ACompletedUploadWithoutProgressCountsTheSizeKnownBefore)
    {
        meter_.setChangesPending(2u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 70u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"));

        endUpload("/watched/a.txt", UploadResult::Completed, 1u, 1u);
        endUpload("/watched/b.txt", UploadResult::Completed, 1u, 0u);

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.changesDone, 2u);
        EXPECT_EQ(progress.bytesDone, 70u);
        EXPECT_EQ(progress.bytesTotal, 70u);
    }

    TEST_F(WatchProgressMeterTest, TheBytesAFailedUploadDidNotSendLeaveTheTotal)
    {
        meter_.setChangesPending(2u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"), 300u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 50u, 100u));
        meter_.uploadProgressed(localPathOf("/watched/b.txt"), fileProgress("/watched/b.txt", 100u, 300u));
        ASSERT_EQ(meter_.snapshot().bytesDone, 150u);
        ASSERT_EQ(meter_.snapshot().bytesTotal, 400u);

        endUpload("/watched/b.txt", UploadResult::NotCompleted, 1u, 1u);

        const auto failed = meter_.snapshot();
        EXPECT_EQ(failed.bytesDone, 150u);
        EXPECT_EQ(failed.bytesTotal, 200u);
        EXPECT_EQ(failed.changesDone, 1u);
        EXPECT_EQ(failed.changesPending, 1u);
        EXPECT_EQ(pathsOf(failed.inFlight), (std::vector<std::string>{"/watched/a.txt"}));

        endUpload("/watched/a.txt", UploadResult::Completed, 1u, 0u);

        const auto ended = meter_.snapshot();
        EXPECT_EQ(ended.bytesDone, 200u);
        EXPECT_EQ(ended.bytesTotal, 200u);
        EXPECT_EQ(ended.changesDone, 2u);
        EXPECT_EQ(ended.changesPending, 0u);
    }

    TEST_F(WatchProgressMeterTest, AFailedUploadThatSentNothingLeavesTheTotalWhole)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 500u);
        ASSERT_EQ(meter_.snapshot().bytesTotal, 500u);

        endUpload("/watched/a.txt", UploadResult::NotCompleted, 1u, 0u);

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.bytesDone, 0u);
        EXPECT_EQ(progress.bytesTotal, 0u);
        EXPECT_EQ(progress.changesDone, 1u);
    }

    TEST_F(WatchProgressMeterTest, TheTotalTheQueueTellsReplacesTheSizeKnownBefore)
    {
        meter_.setChangesPending(2u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"));
        ASSERT_EQ(meter_.snapshot().bytesTotal, 100u);

        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 0u, 150u));
        meter_.uploadProgressed(localPathOf("/watched/b.txt"), fileProgress("/watched/b.txt", 5u, 20u));

        EXPECT_EQ(meter_.snapshot().bytesTotal, 170u);
        EXPECT_EQ(meter_.snapshot().bytesDone, 5u);
    }

    TEST_F(WatchProgressMeterTest, OverlappingUploadsAddUpAndEndOneByOne)
    {
        meter_.setChangesPending(5u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"), 200u);
        meter_.uploadQueued(localPathOf("/watched/c"));
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 30u, 100u));
        meter_.uploadProgressed(
            localPathOf("/watched/c"),
            FileTracking::UploadProgress{
                .bytesSent = 10u,
                .bytesTotal = 50u,
                .inFlight = {fileAt("/watched/c/x.txt", 5u, 20u), fileAt("/watched/c/y.txt", 5u, 30u)},
                .filesDone = 0u,
                .fileCount = 2u,
                .transferred = false,
            }
        );

        const auto overlapping = meter_.snapshot();
        EXPECT_EQ(overlapping.bytesDone, 40u);
        EXPECT_EQ(overlapping.bytesTotal, 350u);
        EXPECT_EQ(
            pathsOf(overlapping.inFlight),
            (std::vector<std::string>{"/watched/a.txt", "/watched/c/x.txt", "/watched/c/y.txt"})
        );
        EXPECT_EQ(overlapping.changesPending, 5u);
        EXPECT_EQ(overlapping.changesWaiting, 2u);
        EXPECT_EQ(overlapping.changesDone, 0u);

        endUpload("/watched/a.txt", UploadResult::Completed, 1u, 4u);

        const auto oneEnded = meter_.snapshot();
        EXPECT_EQ(oneEnded.bytesDone, 110u);
        EXPECT_EQ(oneEnded.bytesTotal, 350u);
        EXPECT_EQ(pathsOf(oneEnded.inFlight), (std::vector<std::string>{"/watched/c/x.txt", "/watched/c/y.txt"}));
        EXPECT_EQ(oneEnded.changesPending, 4u);
        EXPECT_EQ(oneEnded.changesWaiting, 2u);
        EXPECT_EQ(oneEnded.changesDone, 1u);
    }

    TEST_F(WatchProgressMeterTest, TheFilesADirectoryUploadFinishedCountAsDoneBeforeItEnds)
    {
        meter_.setChangesPending(48u);
        meter_.uploadQueued(localPathOf("/watched/dir"));
        meter_.uploadProgressed(
            localPathOf("/watched/dir"),
            FileTracking::UploadProgress{
                .bytesSent = 1200u,
                .bytesTotal = 4800u,
                .inFlight = {fileAt("/watched/dir/13.txt", 50u, 100u), fileAt("/watched/dir/14.txt", 50u, 100u)},
                .filesDone = 12u,
                .fileCount = 48u,
                .transferred = false,
            }
        );

        const auto running = meter_.snapshot();
        EXPECT_EQ(running.changesDone, 12u);
        EXPECT_EQ(running.changesPending, 36u);
        EXPECT_EQ(running.changesWaiting, 34u);
        EXPECT_EQ(running.bytesDone, 1200u);
        EXPECT_EQ(running.bytesTotal, 4800u);
        EXPECT_EQ(running.inFlight.size(), 2u);

        endUpload("/watched/dir", UploadResult::Completed, 48u, 0u);

        const auto ended = meter_.snapshot();
        EXPECT_EQ(ended.changesDone, 48u);
        EXPECT_EQ(ended.changesPending, 0u);
        EXPECT_EQ(ended.changesWaiting, 0u);
        EXPECT_EQ(ended.bytesDone, 4800u);
        EXPECT_EQ(ended.bytesTotal, 4800u);
        EXPECT_TRUE(ended.inFlight.empty());
    }

    TEST_F(WatchProgressMeterTest, AnUploadWhoseBytesAllWentIsDoneButStillOnItsWay)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 90u, 100u, true));

        const auto progress = meter_.snapshot();
        EXPECT_TRUE(progress.inFlight.empty());
        EXPECT_EQ(progress.bytesDone, 100u);
        EXPECT_EQ(progress.bytesTotal, 100u);
        EXPECT_EQ(progress.changesDone, 1u);
        EXPECT_EQ(progress.changesPending, 0u);
        EXPECT_EQ(progress.changesWaiting, 0u);
        EXPECT_FALSE(meter_.isIdle());
        EXPECT_FALSE(meter_.finishBurst(10u));
    }

    TEST_F(WatchProgressMeterTest, ChangesWaitingForAnAnswerArePendingAndWaiting)
    {
        meter_.setChangesPending(3u);
        meter_.setWaitingForAnswer(1u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"), 10u);
        meter_.uploadProgressed(localPathOf("/watched/b.txt"), fileProgress("/watched/b.txt", 1u, 10u));

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.changesPending, 3u);
        EXPECT_EQ(progress.waitingForAnswer, 1u);
        EXPECT_EQ(progress.changesWaiting, 2u);
        EXPECT_FALSE(meter_.isIdle());
    }

    TEST_F(WatchProgressMeterTest, WaitingCountsStayWithinThePendingOnes)
    {
        meter_.setChangesPending(2u);
        meter_.setWaitingForAnswer(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"));
        meter_.uploadQueued(localPathOf("/watched/b.txt"));
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 1u, 10u));
        meter_.uploadProgressed(localPathOf("/watched/b.txt"), fileProgress("/watched/b.txt", 1u, 10u));

        const auto running = meter_.snapshot();
        EXPECT_EQ(running.changesWaiting, 1u);
        EXPECT_EQ(running.waitingForAnswer, 1u);

        meter_.setWaitingForAnswer(5u);

        const auto clamped = meter_.snapshot();
        EXPECT_EQ(clamped.waitingForAnswer, 2u);
        EXPECT_EQ(clamped.changesWaiting, 2u);
        EXPECT_EQ(clamped.changesPending, 2u);
    }

    TEST_F(WatchProgressMeterTest, FinishingABurstKeepsItAsTheLastSyncAndStartsTheNext)
    {
        meter_.setChangesPending(2u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadQueued(localPathOf("/watched/b.txt"), 50u);
        endUpload("/watched/a.txt", UploadResult::Completed, 1u, 1u);
        endUpload("/watched/b.txt", UploadResult::Completed, 1u, 0u);

        EXPECT_TRUE(meter_.finishBurst(1000u));

        const auto idle = meter_.snapshot();
        EXPECT_EQ(idle.changesDone, 0u);
        EXPECT_EQ(idle.changesPending, 0u);
        EXPECT_EQ(idle.bytesDone, 0u);
        EXPECT_EQ(idle.bytesTotal, 0u);
        ASSERT_TRUE(idle.lastSync.has_value());
        EXPECT_EQ(idle.lastSync->endedAt, 1000u);
        EXPECT_EQ(idle.lastSync->changes, 2u);
        EXPECT_EQ(idle.lastSync->bytes, 150u);

        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/c.txt"), 10u);

        const auto next = meter_.snapshot();
        EXPECT_EQ(next.bytesTotal, 10u);
        EXPECT_EQ(next.changesDone, 0u);
        ASSERT_TRUE(next.lastSync.has_value());
        EXPECT_EQ(next.lastSync->endedAt, 1000u);

        endUpload("/watched/c.txt", UploadResult::Completed, 1u, 0u);
        EXPECT_TRUE(meter_.finishBurst(2000u));

        const auto second = meter_.snapshot().lastSync;
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ(second->endedAt, 2000u);
        EXPECT_EQ(second->changes, 1u);
        EXPECT_EQ(second->bytes, 10u);
    }

    TEST_F(WatchProgressMeterTest, ABurstDoesNotFinishWhileSomethingIsOnItsWay)
    {
        meter_.changesEnded(1u);

        meter_.setChangesPending(1u);
        EXPECT_FALSE(meter_.isIdle());
        EXPECT_FALSE(meter_.finishBurst(10u));

        meter_.setChangesPending(0u);
        meter_.setWaitingForAnswer(1u);
        EXPECT_FALSE(meter_.isIdle());
        EXPECT_FALSE(meter_.finishBurst(10u));

        meter_.setWaitingForAnswer(0u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 10u);
        EXPECT_FALSE(meter_.isIdle());
        EXPECT_FALSE(meter_.finishBurst(10u));

        EXPECT_FALSE(meter_.snapshot().lastSync.has_value());
        EXPECT_EQ(meter_.snapshot().changesDone, 1u);
    }

    TEST_F(WatchProgressMeterTest, ABurstInWhichNothingEndedLeavesTheLastSync)
    {
        EXPECT_FALSE(meter_.finishBurst(10u));
        EXPECT_FALSE(meter_.snapshot().lastSync.has_value());

        meter_.changesEnded(3u);
        ASSERT_TRUE(meter_.finishBurst(20u));
        EXPECT_FALSE(meter_.finishBurst(30u));

        const auto lastSync = meter_.snapshot().lastSync;
        ASSERT_TRUE(lastSync.has_value());
        EXPECT_EQ(lastSync->endedAt, 20u);
        EXPECT_EQ(lastSync->changes, 3u);
        EXPECT_EQ(lastSync->bytes, 0u);
    }

    TEST_F(WatchProgressMeterTest, ABurstOfAFailedUploadKeepsTheBytesItSent)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 30u, 100u));
        endUpload("/watched/a.txt", UploadResult::NotCompleted, 1u, 0u);

        ASSERT_TRUE(meter_.finishBurst(5u));

        const auto lastSync = meter_.snapshot().lastSync;
        ASSERT_TRUE(lastSync.has_value());
        EXPECT_EQ(lastSync->changes, 1u);
        EXPECT_EQ(lastSync->bytes, 30u);
    }

    TEST_F(WatchProgressMeterTest, ProgressOfAnUploadThatIsNotOnItsWayIsIgnored)
    {
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 30u, 100u));
        EXPECT_EQ(meter_.snapshot().bytesTotal, 0u);
        EXPECT_TRUE(meter_.snapshot().inFlight.empty());
        EXPECT_FALSE(meter_.isDirty());

        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadEnded(localPathOf("/watched/a.txt"), UploadResult::Completed);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 30u, 100u));

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.bytesDone, 100u);
        EXPECT_EQ(progress.bytesTotal, 100u);
        EXPECT_TRUE(progress.inFlight.empty());
        EXPECT_TRUE(meter_.isIdle());
    }

    TEST_F(WatchProgressMeterTest, QueueingAPathAgainEndsItsUploadOnItsWayWithoutCompleting)
    {
        meter_.setChangesPending(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 40u, 100u));

        meter_.uploadQueued(localPathOf("/watched/a.txt"), 200u);

        const auto progress = meter_.snapshot();
        EXPECT_EQ(progress.bytesDone, 40u);
        EXPECT_EQ(progress.bytesTotal, 240u);
        EXPECT_TRUE(progress.inFlight.empty());
        EXPECT_EQ(progress.changesWaiting, 1u);
    }

    TEST_F(WatchProgressMeterTest, TheLateEndOfAnUploadQueuedAgainLeavesTheNewOneOnItsWay)
    {
        meter_.setChangesPending(1u);
        const auto first = meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        const auto second = meter_.uploadQueued(localPathOf("/watched/a.txt"), 200u);

        meter_.uploadEnded(localPathOf("/watched/a.txt"), UploadResult::NotCompleted, first);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 50u, 200u), first);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 80u, 200u), second);

        const auto progress = meter_.snapshot();
        EXPECT_FALSE(meter_.isIdle());
        EXPECT_EQ(progress.bytesDone, 80u);
        EXPECT_EQ(progress.bytesTotal, 200u);
        ASSERT_EQ(progress.inFlight.size(), 1u);
        EXPECT_EQ(progress.inFlight.front().bytes, 80u);
    }

    TEST_F(WatchProgressMeterTest, DroppingWhatIsOnItsWayLetsTheBurstFinish)
    {
        meter_.changesEnded(1u);
        meter_.setChangesPending(3u);
        meter_.setWaitingForAnswer(1u);
        meter_.uploadQueued(localPathOf("/watched/a.txt"), 100u);
        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 60u, 100u));
        meter_.takeDirty();

        meter_.dropPending();

        EXPECT_TRUE(meter_.isDirty());
        EXPECT_TRUE(meter_.isIdle());
        const auto dropped = meter_.snapshot();
        EXPECT_EQ(dropped.changesPending, 0u);
        EXPECT_EQ(dropped.changesWaiting, 0u);
        EXPECT_EQ(dropped.waitingForAnswer, 0u);
        EXPECT_EQ(dropped.changesDone, 1u);
        EXPECT_EQ(dropped.bytesDone, 60u);
        EXPECT_EQ(dropped.bytesTotal, 60u);
        EXPECT_TRUE(dropped.inFlight.empty());

        ASSERT_TRUE(meter_.finishBurst(9u));
        const auto lastSync = meter_.snapshot().lastSync;
        ASSERT_TRUE(lastSync.has_value());
        EXPECT_EQ(lastSync->endedAt, 9u);
        EXPECT_EQ(lastSync->changes, 1u);
        EXPECT_EQ(lastSync->bytes, 60u);
    }

    TEST_F(WatchProgressMeterTest, DroppingNothingLeavesTheMeterClean)
    {
        meter_.dropPending();

        EXPECT_FALSE(meter_.isDirty());
        EXPECT_TRUE(meter_.isIdle());
    }

    TEST_F(WatchProgressMeterTest, TheDirtyFlagTellsOfChangesUntilItIsTaken)
    {
        EXPECT_FALSE(meter_.takeDirty());

        meter_.setChangesPending(0u);
        meter_.setWaitingForAnswer(0u);
        meter_.changesEnded(0u);
        EXPECT_FALSE(meter_.isDirty());

        meter_.setChangesPending(1u);
        EXPECT_TRUE(meter_.isDirty());
        EXPECT_TRUE(meter_.isDirty());
        EXPECT_TRUE(meter_.takeDirty());
        EXPECT_FALSE(meter_.isDirty());
        EXPECT_FALSE(meter_.takeDirty());

        meter_.setChangesPending(1u);
        EXPECT_FALSE(meter_.isDirty());

        meter_.uploadQueued(localPathOf("/watched/a.txt"), 10u);
        EXPECT_TRUE(meter_.takeDirty());

        meter_.uploadProgressed(localPathOf("/watched/b.txt"), fileProgress("/watched/b.txt", 1u, 10u));
        meter_.uploadEnded(localPathOf("/watched/b.txt"), UploadResult::Completed);
        EXPECT_FALSE(meter_.isDirty());

        meter_.uploadProgressed(localPathOf("/watched/a.txt"), fileProgress("/watched/a.txt", 1u, 10u));
        EXPECT_TRUE(meter_.takeDirty());

        meter_.setWaitingForAnswer(1u);
        EXPECT_TRUE(meter_.takeDirty());

        EXPECT_FALSE(meter_.finishBurst(5u));
        EXPECT_FALSE(meter_.isDirty());

        meter_.uploadEnded(localPathOf("/watched/a.txt"), UploadResult::Completed);
        EXPECT_TRUE(meter_.takeDirty());

        meter_.changesEnded(1u);
        EXPECT_TRUE(meter_.takeDirty());

        meter_.setChangesPending(0u);
        meter_.setWaitingForAnswer(0u);
        meter_.takeDirty();
        EXPECT_TRUE(meter_.finishBurst(5u));
        EXPECT_TRUE(meter_.takeDirty());
    }
}
