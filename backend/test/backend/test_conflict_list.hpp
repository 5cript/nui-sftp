#pragma once

#include <backend/file_tracking/conflict_list.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>
#include <shared_data/file_tracking/watch_conflict.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Test::ConflictListCases
{
    /**
     * @brief Stands for what the tracker keeps with a conflict.
     */
    struct WaitingUpload
    {
        std::string label{};
    };

    using Conflicts = FileTracking::ConflictList<WaitingUpload>;

    inline Paths::LocalPath localPathOf(std::string_view path)
    {
        return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
    }

    inline SharedData::WatchConflict conflictAt(
        Ids::WatchId const& watchId,
        std::string_view path,
        SharedData::ConflictKind kind = SharedData::ConflictKind::ChangedOnServer,
        std::uint64_t raisedAt = 100u
    )
    {
        return SharedData::WatchConflict{
            .conflictId = Ids::makeConflictId("chosen-by-the-caller"),
            .watchId = watchId,
            .kind = kind,
            .localPath = localPathOf(path),
            .remotePath = Paths::RemotePath::make(fmt::format("/srv{}", path)).value(),
            .serverModifiedAt = 50u,
            .localModifiedAt = 60u,
            .raisedAt = raisedAt,
        };
    }

    inline std::vector<std::string> pathsOf(std::vector<SharedData::WatchConflict> const& conflicts)
    {
        std::vector<std::string> paths;
        for (auto const& conflict : conflicts)
            paths.push_back(conflict.localPath.toUtf8());
        return paths;
    }

    inline std::vector<std::string> labelsOf(std::vector<Conflicts::Listed> const& listed)
    {
        std::vector<std::string> labels;
        for (auto const& entry : listed)
            labels.push_back(entry.context.label);
        return labels;
    }

    class ConflictListTest : public ::testing::Test
    {
      protected:
        Conflicts conflicts_{};
        Ids::WatchId watch_{Ids::makeWatchId("watch-one")};
        Ids::WatchId otherWatch_{Ids::makeWatchId("watch-two")};
    };

    TEST_F(ConflictListTest, ANewConflictGetsAnIdOfItsOwnAndIsFoundByIdAndByPath)
    {
        const auto added = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "a"});

        EXPECT_TRUE(added.conflict.conflictId.isValid());
        EXPECT_NE(added.conflict.conflictId.value(), "chosen-by-the-caller");
        EXPECT_FALSE(added.replaced.has_value());

        const auto* byId = conflicts_.find(added.conflict.conflictId);
        ASSERT_NE(byId, nullptr);
        EXPECT_EQ(byId->context.label, "a");
        EXPECT_EQ(byId->conflict.conflictId, added.conflict.conflictId);

        const auto* byPath = conflicts_.find(watch_, localPathOf("/watched/a.txt"));
        ASSERT_NE(byPath, nullptr);
        EXPECT_EQ(byPath->conflict.conflictId, added.conflict.conflictId);

        EXPECT_EQ(conflicts_.size(), 1u);
        EXPECT_FALSE(conflicts_.empty());
        EXPECT_EQ(conflicts_.countOf(watch_), 1u);
    }

    TEST_F(ConflictListTest, AListedConflictKeepsWhatItWasAddedWith)
    {
        const auto added = conflicts_.add(
            conflictAt(watch_, "/watched/a.txt", SharedData::ConflictKind::DeletedOnServer, 123u), WaitingUpload{}
        );

        const auto* listed = conflicts_.find(added.conflict.conflictId);
        ASSERT_NE(listed, nullptr);
        EXPECT_EQ(listed->conflict.watchId, watch_);
        EXPECT_EQ(listed->conflict.kind, SharedData::ConflictKind::DeletedOnServer);
        EXPECT_EQ(listed->conflict.localPath.toUtf8(), "/watched/a.txt");
        EXPECT_EQ(listed->conflict.remotePath.value(), "/srv/watched/a.txt");
        EXPECT_EQ(listed->conflict.serverModifiedAt, std::optional<std::uint64_t>{50u});
        EXPECT_EQ(listed->conflict.localModifiedAt, std::optional<std::uint64_t>{60u});
        EXPECT_EQ(listed->conflict.raisedAt, 123u);
    }

    TEST_F(ConflictListTest, ANewerConflictOfTheSamePathReplacesItAndKeepsItsId)
    {
        const auto first = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "first"});
        const auto second = conflicts_.add(
            conflictAt(watch_, "/watched/a.txt", SharedData::ConflictKind::CreatedOnServer, 200u),
            WaitingUpload{.label = "second"}
        );

        EXPECT_EQ(second.conflict.conflictId, first.conflict.conflictId);
        ASSERT_TRUE(second.replaced.has_value());
        EXPECT_EQ(second.replaced->label, "first");

        const auto* listed = conflicts_.find(first.conflict.conflictId);
        ASSERT_NE(listed, nullptr);
        EXPECT_EQ(listed->context.label, "second");
        EXPECT_EQ(listed->conflict.kind, SharedData::ConflictKind::CreatedOnServer);
        EXPECT_EQ(listed->conflict.raisedAt, 200u);
        EXPECT_EQ(conflicts_.size(), 1u);
        EXPECT_EQ(conflicts_.countOf(watch_), 1u);
    }

    TEST_F(ConflictListTest, TheSamePathOfAnotherWatchIsAConflictOfItsOwn)
    {
        const auto mine = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "mine"});
        const auto other = conflicts_.add(conflictAt(otherWatch_, "/watched/a.txt"), WaitingUpload{.label = "other"});

        EXPECT_NE(mine.conflict.conflictId, other.conflict.conflictId);
        EXPECT_FALSE(other.replaced.has_value());
        EXPECT_EQ(conflicts_.countOf(watch_), 1u);
        EXPECT_EQ(conflicts_.countOf(otherWatch_), 1u);
        const auto* listed = conflicts_.find(otherWatch_, localPathOf("/watched/a.txt"));
        ASSERT_NE(listed, nullptr);
        EXPECT_EQ(listed->context.label, "other");
    }

    TEST_F(ConflictListTest, TheConflictsOfAWatchComeInTheOrderTheyArrived)
    {
        conflicts_.add(conflictAt(watch_, "/watched/c.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(otherWatch_, "/other/x.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{});

        EXPECT_EQ(
            pathsOf(conflicts_.conflictsOf(watch_)),
            (std::vector<std::string>{"/watched/c.txt", "/watched/a.txt", "/watched/b.txt"})
        );
        EXPECT_EQ(pathsOf(conflicts_.conflictsOf(otherWatch_)), (std::vector<std::string>{"/other/x.txt"}));
    }

    TEST_F(ConflictListTest, AReplacedConflictArrivesAnewBehindTheOthers)
    {
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/c.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});

        EXPECT_EQ(
            pathsOf(conflicts_.conflictsOf(watch_)),
            (std::vector<std::string>{"/watched/b.txt", "/watched/c.txt", "/watched/a.txt"})
        );
    }

    TEST_F(ConflictListTest, RemovingAConflictByIdHandsBackItsContext)
    {
        const auto kept = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "kept"});
        const auto added = conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{.label = "answered"});

        const auto removed = conflicts_.remove(added.conflict.conflictId);

        ASSERT_TRUE(removed.has_value());
        EXPECT_EQ(removed->context.label, "answered");
        EXPECT_EQ(removed->conflict.conflictId, added.conflict.conflictId);
        EXPECT_EQ(conflicts_.find(added.conflict.conflictId), nullptr);
        EXPECT_EQ(conflicts_.find(watch_, localPathOf("/watched/b.txt")), nullptr);
        EXPECT_NE(conflicts_.find(kept.conflict.conflictId), nullptr);
        EXPECT_EQ(conflicts_.countOf(watch_), 1u);
        EXPECT_EQ(pathsOf(conflicts_.conflictsOf(watch_)), (std::vector<std::string>{"/watched/a.txt"}));
    }

    TEST_F(ConflictListTest, RemovingAConflictTwiceOrAnUnknownOneRemovesNothing)
    {
        const auto added = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});

        EXPECT_FALSE(conflicts_.remove(Ids::makeConflictId("unknown")).has_value());
        EXPECT_EQ(conflicts_.size(), 1u);
        EXPECT_TRUE(conflicts_.remove(added.conflict.conflictId).has_value());
        EXPECT_FALSE(conflicts_.remove(added.conflict.conflictId).has_value());
        EXPECT_TRUE(conflicts_.empty());
        EXPECT_EQ(conflicts_.countOf(watch_), 0u);
        EXPECT_TRUE(conflicts_.conflictsOf(watch_).empty());
    }

    TEST_F(ConflictListTest, ARemovedPathGetsANewIdWhenItConflictsAgain)
    {
        const auto first = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.remove(first.conflict.conflictId);

        const auto again = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});

        EXPECT_NE(again.conflict.conflictId, first.conflict.conflictId);
        EXPECT_FALSE(again.replaced.has_value());
    }

    TEST_F(ConflictListTest, DroppingAWatchRemovesOnlyItsConflictsInTheOrderTheyArrived)
    {
        const auto first = conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{.label = "first"});
        const auto other = conflicts_.add(conflictAt(otherWatch_, "/other/x.txt"), WaitingUpload{.label = "other"});
        const auto second = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "second"});

        const auto dropped = conflicts_.removeWatch(watch_);

        EXPECT_EQ(labelsOf(dropped), (std::vector<std::string>{"first", "second"}));
        ASSERT_EQ(dropped.size(), 2u);
        EXPECT_EQ(dropped.front().conflict.conflictId, first.conflict.conflictId);
        EXPECT_EQ(dropped.back().conflict.conflictId, second.conflict.conflictId);
        EXPECT_EQ(conflicts_.find(first.conflict.conflictId), nullptr);
        EXPECT_EQ(conflicts_.find(watch_, localPathOf("/watched/a.txt")), nullptr);
        EXPECT_EQ(conflicts_.countOf(watch_), 0u);
        EXPECT_TRUE(conflicts_.conflictsOf(watch_).empty());
        EXPECT_NE(conflicts_.find(other.conflict.conflictId), nullptr);
        EXPECT_EQ(conflicts_.countOf(otherWatch_), 1u);
        EXPECT_EQ(conflicts_.size(), 1u);
        EXPECT_TRUE(conflicts_.removeWatch(watch_).empty());
    }

    TEST_F(ConflictListTest, ADroppedWatchStartsAfreshWithNewIds)
    {
        const auto before = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.removeWatch(watch_);

        const auto after = conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});

        EXPECT_NE(after.conflict.conflictId, before.conflict.conflictId);
        EXPECT_FALSE(after.replaced.has_value());
        EXPECT_EQ(conflicts_.countOf(watch_), 1u);
    }

    TEST_F(ConflictListTest, RemovingAllHandsBackEveryConflictInTheOrderTheyArrived)
    {
        conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{.label = "first"});
        conflicts_.add(conflictAt(otherWatch_, "/other/x.txt"), WaitingUpload{.label = "second"});
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{.label = "third"});
        conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{.label = "fourth"});

        const auto removed = conflicts_.removeAll();

        EXPECT_EQ(labelsOf(removed), (std::vector<std::string>{"second", "third", "fourth"}));
        EXPECT_TRUE(conflicts_.empty());
        EXPECT_EQ(conflicts_.size(), 0u);
        EXPECT_EQ(conflicts_.countOf(watch_), 0u);
        EXPECT_EQ(conflicts_.countOf(otherWatch_), 0u);
        EXPECT_TRUE(conflicts_.removeAll().empty());
    }

    TEST_F(ConflictListTest, CountsAreKeptPerWatch)
    {
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/b.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});
        conflicts_.add(conflictAt(otherWatch_, "/other/x.txt"), WaitingUpload{});

        EXPECT_EQ(conflicts_.countOf(watch_), 2u);
        EXPECT_EQ(conflicts_.countOf(otherWatch_), 1u);
        EXPECT_EQ(conflicts_.countOf(Ids::makeWatchId("unknown")), 0u);
        EXPECT_EQ(conflicts_.size(), 3u);
    }

    TEST_F(ConflictListTest, NothingIsFoundForAnUnknownWatchPathOrId)
    {
        conflicts_.add(conflictAt(watch_, "/watched/a.txt"), WaitingUpload{});

        EXPECT_EQ(conflicts_.find(Ids::makeConflictId("unknown")), nullptr);
        EXPECT_EQ(conflicts_.find(watch_, localPathOf("/watched/b.txt")), nullptr);
        EXPECT_EQ(conflicts_.find(otherWatch_, localPathOf("/watched/a.txt")), nullptr);
        EXPECT_TRUE(conflicts_.conflictsOf(otherWatch_).empty());
    }
}
