#pragma once

#include <backend/file_tracking/failure_list.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/upload_failure.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Test::FailureListCases
{
    inline Paths::LocalPath localPathOf(std::string_view path)
    {
        return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
    }

    inline SharedData::UploadFailure failureAt(
        Ids::WatchId const& watchId,
        std::string_view path,
        SharedData::UploadFailureReason reason = SharedData::UploadFailureReason::Unknown,
        std::uint64_t failedAt = 10u,
        std::optional<std::string> detail = std::nullopt
    )
    {
        return SharedData::UploadFailure{
            .failureId = Ids::makeFailureId("chosen-by-the-caller"),
            .watchId = watchId,
            .localPath = localPathOf(path),
            .remotePath = Paths::RemotePath::make(fmt::format("/srv{}", path)).value(),
            .reason = reason,
            .detail = std::move(detail),
            .failedAt = failedAt,
            .attempts = 7u,
        };
    }

    inline std::vector<std::string> pathsOf(std::vector<SharedData::UploadFailure> const& failures)
    {
        std::vector<std::string> paths;
        for (auto const& failure : failures)
            paths.push_back(failure.localPath.toUtf8());
        return paths;
    }

    class FailureListTest : public ::testing::Test
    {
      protected:
        FileTracking::FailureList failures_{};
        Ids::WatchId watch_{Ids::makeWatchId("watch-one")};
        Ids::WatchId otherWatch_{Ids::makeWatchId("watch-two")};
    };

    TEST_F(FailureListTest, ANewFailureGetsAnIdOfItsOwnAndOneAttempt)
    {
        const auto recorded = failures_.record(failureAt(watch_, "/watched/a.txt"));

        EXPECT_TRUE(recorded.failureId.isValid());
        EXPECT_NE(recorded.failureId.value(), "chosen-by-the-caller");
        EXPECT_EQ(recorded.attempts, 1u);

        const auto* byId = failures_.find(recorded.failureId);
        ASSERT_NE(byId, nullptr);
        EXPECT_EQ(byId->localPath.toUtf8(), "/watched/a.txt");
        EXPECT_EQ(byId->attempts, 1u);

        const auto* byPath = failures_.find(watch_, localPathOf("/watched/a.txt"));
        ASSERT_NE(byPath, nullptr);
        EXPECT_EQ(byPath->failureId, recorded.failureId);

        EXPECT_EQ(failures_.size(), 1u);
        EXPECT_FALSE(failures_.empty());
        EXPECT_EQ(failures_.countOf(watch_), 1u);
    }

    TEST_F(FailureListTest, TheSamePathFailingAgainKeepsItsIdTakesTheNewReasonAndCountsAttempts)
    {
        const auto first = failures_.record(
            failureAt(watch_, "/watched/a.txt", SharedData::UploadFailureReason::LockedLocally, 10u, "busy")
        );
        auto again = failureAt(watch_, "/watched/a.txt", SharedData::UploadFailureReason::ServerRefused, 20u, "denied");
        again.remotePath = Paths::RemotePath::make("/srv/renamed/a.txt").value();
        const auto second = failures_.record(std::move(again));

        EXPECT_EQ(second.failureId, first.failureId);
        EXPECT_EQ(second.attempts, 2u);
        EXPECT_EQ(second.reason, SharedData::UploadFailureReason::ServerRefused);
        EXPECT_EQ(second.detail, std::optional<std::string>{"denied"});
        EXPECT_EQ(second.failedAt, 20u);
        EXPECT_EQ(second.remotePath.value(), "/srv/renamed/a.txt");

        const auto* listed = failures_.find(first.failureId);
        ASSERT_NE(listed, nullptr);
        EXPECT_EQ(listed->attempts, 2u);
        EXPECT_EQ(listed->reason, SharedData::UploadFailureReason::ServerRefused);
        EXPECT_EQ(listed->failedAt, 20u);

        const auto third = failures_.record(failureAt(watch_, "/watched/a.txt"));
        EXPECT_EQ(third.failureId, first.failureId);
        EXPECT_EQ(third.attempts, 3u);
        EXPECT_EQ(failures_.size(), 1u);
        EXPECT_EQ(failures_.countOf(watch_), 1u);
    }

    TEST_F(FailureListTest, AFailureWithoutDetailDropsTheDetailOfTheOneBefore)
    {
        failures_.record(failureAt(watch_, "/watched/a.txt", SharedData::UploadFailureReason::Unknown, 10u, "old"));
        const auto again = failures_.record(failureAt(watch_, "/watched/a.txt"));

        EXPECT_FALSE(again.detail.has_value());
        EXPECT_FALSE(failures_.find(again.failureId)->detail.has_value());
    }

    TEST_F(FailureListTest, TheSamePathOfAnotherWatchIsAFailureOfItsOwn)
    {
        const auto mine = failures_.record(failureAt(watch_, "/watched/a.txt"));
        const auto other = failures_.record(failureAt(otherWatch_, "/watched/a.txt"));

        EXPECT_NE(mine.failureId, other.failureId);
        EXPECT_EQ(other.attempts, 1u);
        EXPECT_EQ(failures_.countOf(watch_), 1u);
        EXPECT_EQ(failures_.countOf(otherWatch_), 1u);
    }

    TEST_F(FailureListTest, TheFailuresOfAWatchComeInTheOrderTheirPathsFirstFailed)
    {
        failures_.record(failureAt(watch_, "/watched/c.txt"));
        failures_.record(failureAt(otherWatch_, "/other/x.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));
        failures_.record(failureAt(watch_, "/watched/c.txt"));

        EXPECT_EQ(
            pathsOf(failures_.failuresOf(watch_)),
            (std::vector<std::string>{"/watched/c.txt", "/watched/a.txt", "/watched/b.txt"})
        );
        EXPECT_EQ(pathsOf(failures_.failuresOf(otherWatch_)), (std::vector<std::string>{"/other/x.txt"}));
    }

    TEST_F(FailureListTest, APathThatUploadedLosesItsFailureAndStartsAfreshWhenItFailsAgain)
    {
        const auto first = failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));

        const auto removed = failures_.removePath(watch_, localPathOf("/watched/a.txt"));

        ASSERT_TRUE(removed.has_value());
        EXPECT_EQ(removed->failureId, first.failureId);
        EXPECT_EQ(removed->attempts, 2u);
        EXPECT_EQ(failures_.find(first.failureId), nullptr);
        EXPECT_EQ(failures_.find(watch_, localPathOf("/watched/a.txt")), nullptr);
        EXPECT_EQ(failures_.countOf(watch_), 1u);
        EXPECT_FALSE(failures_.removePath(watch_, localPathOf("/watched/a.txt")).has_value());
        EXPECT_FALSE(failures_.removePath(otherWatch_, localPathOf("/watched/b.txt")).has_value());

        const auto again = failures_.record(failureAt(watch_, "/watched/a.txt"));
        EXPECT_NE(again.failureId, first.failureId);
        EXPECT_EQ(again.attempts, 1u);
        EXPECT_EQ(
            pathsOf(failures_.failuresOf(watch_)), (std::vector<std::string>{"/watched/b.txt", "/watched/a.txt"})
        );
    }

    TEST_F(FailureListTest, DismissingAFailureByIdTakesOnlyItOff)
    {
        const auto dismissed = failures_.record(failureAt(watch_, "/watched/a.txt"));
        const auto kept = failures_.record(failureAt(watch_, "/watched/b.txt"));

        const auto removed = failures_.remove(dismissed.failureId);

        ASSERT_TRUE(removed.has_value());
        EXPECT_EQ(removed->localPath.toUtf8(), "/watched/a.txt");
        EXPECT_EQ(failures_.find(dismissed.failureId), nullptr);
        EXPECT_NE(failures_.find(kept.failureId), nullptr);
        EXPECT_EQ(failures_.countOf(watch_), 1u);
        EXPECT_FALSE(failures_.remove(dismissed.failureId).has_value());
        EXPECT_FALSE(failures_.remove(Ids::makeFailureId("unknown")).has_value());
        EXPECT_EQ(failures_.size(), 1u);
    }

    TEST_F(FailureListTest, ADirectoryThatUploadedTakesItsFailuresAndThoseBelowOffButTheOnesItLeftOut)
    {
        failures_.record(failureAt(watch_, "/watched/dir/sub/b.txt"));
        failures_.record(failureAt(watch_, "/watched/dir"));
        failures_.record(failureAt(watch_, "/watched/dir-sibling.txt"));
        failures_.record(failureAt(watch_, "/watched/dir/a.txt"));
        failures_.record(failureAt(watch_, "/watched/other.txt"));
        failures_.record(failureAt(otherWatch_, "/watched/dir/a.txt"));

        const auto removed = failures_.removeWithin(
            watch_, localPathOf("/watched/dir"), std::set<Paths::LocalPath>{localPathOf("/watched/dir/sub/b.txt")}
        );

        EXPECT_EQ(pathsOf(removed), (std::vector<std::string>{"/watched/dir", "/watched/dir/a.txt"}));
        EXPECT_EQ(
            pathsOf(failures_.failuresOf(watch_)),
            (std::vector<std::string>{"/watched/dir/sub/b.txt", "/watched/dir-sibling.txt", "/watched/other.txt"})
        );
        EXPECT_EQ(failures_.countOf(otherWatch_), 1u);
    }

    TEST_F(FailureListTest, RemovingWithinADirectoryWithoutExceptionsTakesEverythingBelowIt)
    {
        failures_.record(failureAt(watch_, "/watched/dir/a.txt"));
        failures_.record(failureAt(watch_, "/watched/dir/sub/b.txt"));
        failures_.record(failureAt(watch_, "/watched/other.txt"));

        const auto removed = failures_.removeWithin(watch_, localPathOf("/watched/dir"));

        EXPECT_EQ(pathsOf(removed), (std::vector<std::string>{"/watched/dir/a.txt", "/watched/dir/sub/b.txt"}));
        EXPECT_EQ(pathsOf(failures_.failuresOf(watch_)), (std::vector<std::string>{"/watched/other.txt"}));
        EXPECT_TRUE(failures_.removeWithin(otherWatch_, localPathOf("/watched")).empty());
    }

    TEST_F(FailureListTest, DroppingAWatchRemovesOnlyItsFailuresInTheOrderTheyFirstFailed)
    {
        const auto first = failures_.record(failureAt(watch_, "/watched/b.txt"));
        const auto other = failures_.record(failureAt(otherWatch_, "/other/x.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));

        const auto dropped = failures_.removeWatch(watch_);

        EXPECT_EQ(pathsOf(dropped), (std::vector<std::string>{"/watched/b.txt", "/watched/a.txt"}));
        ASSERT_FALSE(dropped.empty());
        EXPECT_EQ(dropped.front().failureId, first.failureId);
        EXPECT_EQ(dropped.front().attempts, 2u);
        EXPECT_EQ(failures_.find(first.failureId), nullptr);
        EXPECT_EQ(failures_.countOf(watch_), 0u);
        EXPECT_TRUE(failures_.failuresOf(watch_).empty());
        EXPECT_NE(failures_.find(other.failureId), nullptr);
        EXPECT_EQ(failures_.size(), 1u);
        EXPECT_TRUE(failures_.removeWatch(watch_).empty());
    }

    TEST_F(FailureListTest, RemovingAllHandsBackEveryFailureInTheOrderTheyFirstFailed)
    {
        failures_.record(failureAt(watch_, "/watched/b.txt"));
        failures_.record(failureAt(otherWatch_, "/other/x.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));

        const auto removed = failures_.removeAll();

        EXPECT_EQ(pathsOf(removed), (std::vector<std::string>{"/watched/b.txt", "/other/x.txt", "/watched/a.txt"}));
        EXPECT_TRUE(failures_.empty());
        EXPECT_EQ(failures_.countOf(watch_), 0u);
        EXPECT_EQ(failures_.countOf(otherWatch_), 0u);
        EXPECT_TRUE(failures_.removeAll().empty());
    }

    TEST_F(FailureListTest, ARetriedFailureLeavesTheListAndComesBackWithItsIdAndPlaceWhenItFailsAgain)
    {
        const auto first = failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));

        const auto retried = failures_.retry(first.failureId);

        ASSERT_TRUE(retried.has_value());
        EXPECT_EQ(retried->failureId, first.failureId);
        EXPECT_EQ(retried->attempts, 2u);
        EXPECT_EQ(failures_.find(first.failureId), nullptr);
        EXPECT_EQ(failures_.find(watch_, localPathOf("/watched/a.txt")), nullptr);
        EXPECT_EQ(failures_.countOf(watch_), 1u);
        EXPECT_FALSE(failures_.retry(first.failureId).has_value());
        EXPECT_FALSE(failures_.remove(first.failureId).has_value());

        const auto again =
            failures_.record(failureAt(watch_, "/watched/a.txt", SharedData::UploadFailureReason::LockedLocally));
        EXPECT_EQ(again.failureId, first.failureId);
        EXPECT_EQ(again.attempts, 3u);
        EXPECT_EQ(again.reason, SharedData::UploadFailureReason::LockedLocally);
        EXPECT_EQ(
            pathsOf(failures_.failuresOf(watch_)), (std::vector<std::string>{"/watched/a.txt", "/watched/b.txt"})
        );
    }

    TEST_F(FailureListTest, ARetriedPathThatUploadedStartsAfreshWhenItFailsAgain)
    {
        const auto first = failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.retry(first.failureId);

        EXPECT_FALSE(failures_.removePath(watch_, localPathOf("/watched/a.txt")).has_value());

        const auto again = failures_.record(failureAt(watch_, "/watched/a.txt"));
        EXPECT_NE(again.failureId, first.failureId);
        EXPECT_EQ(again.attempts, 1u);
    }

    TEST_F(FailureListTest, RetriedFailuresAreForgottenWithinAnUploadedDirectoryAndWithTheirWatch)
    {
        const auto below = failures_.record(failureAt(watch_, "/watched/dir/a.txt"));
        const auto leftOut = failures_.record(failureAt(watch_, "/watched/dir/b.txt"));
        const auto elsewhere = failures_.record(failureAt(watch_, "/watched/other.txt"));
        failures_.retry(below.failureId);
        failures_.retry(leftOut.failureId);
        failures_.retry(elsewhere.failureId);

        EXPECT_TRUE(failures_
                .removeWithin(
                    watch_, localPathOf("/watched/dir"), std::set<Paths::LocalPath>{localPathOf("/watched/dir/b.txt")}
                )
                .empty());

        EXPECT_NE(failures_.record(failureAt(watch_, "/watched/dir/a.txt")).failureId, below.failureId);
        EXPECT_EQ(failures_.record(failureAt(watch_, "/watched/dir/b.txt")).failureId, leftOut.failureId);
        EXPECT_EQ(failures_.removeWatch(watch_).size(), 2u);
        EXPECT_NE(failures_.record(failureAt(watch_, "/watched/other.txt")).failureId, elsewhere.failureId);
    }

    TEST_F(FailureListTest, RemovingAllForgetsTheRetriedFailures)
    {
        const auto retried = failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.retry(retried.failureId);

        EXPECT_TRUE(failures_.removeAll().empty());

        EXPECT_NE(failures_.record(failureAt(watch_, "/watched/a.txt")).failureId, retried.failureId);
    }

    TEST_F(FailureListTest, CountsAreKeptPerWatch)
    {
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(watch_, "/watched/b.txt"));
        failures_.record(failureAt(watch_, "/watched/a.txt"));
        failures_.record(failureAt(otherWatch_, "/other/x.txt"));

        EXPECT_EQ(failures_.countOf(watch_), 2u);
        EXPECT_EQ(failures_.countOf(otherWatch_), 1u);
        EXPECT_EQ(failures_.countOf(Ids::makeWatchId("unknown")), 0u);
        EXPECT_EQ(failures_.size(), 3u);
    }

    TEST_F(FailureListTest, NothingIsFoundForAnUnknownWatchPathOrId)
    {
        failures_.record(failureAt(watch_, "/watched/a.txt"));

        EXPECT_EQ(failures_.find(Ids::makeFailureId("unknown")), nullptr);
        EXPECT_EQ(failures_.find(watch_, localPathOf("/watched/b.txt")), nullptr);
        EXPECT_EQ(failures_.find(otherWatch_, localPathOf("/watched/a.txt")), nullptr);
        EXPECT_TRUE(failures_.failuresOf(otherWatch_).empty());
    }
}
