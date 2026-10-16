#pragma once

#include <backend/sync/sync_session.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    namespace SyncSessionTestDetail
    {
        using namespace SharedData::Sync;

        inline ScanNode file(std::string name, std::uint64_t size, std::uint64_t mtime)
        {
            return ScanNode{
                .name = std::move(name), .type = SharedData::FileType::Regular, .size = size, .mtime = mtime
            };
        }

        inline ScanNode directory(std::string name, std::vector<ScanNode> children)
        {
            std::sort(
                children.begin(),
                children.end(),
                [](auto const& lhs, auto const& rhs)
                {
                    return lhs.name < rhs.name;
                }
            );
            ScanNode node{
                .name = std::move(name), .type = SharedData::FileType::Directory, .children = std::move(children)
            };
            for (auto const& child : node.children)
            {
                const bool isDirectory = child.type == SharedData::FileType::Directory;
                node.subtreeFileCount += isDirectory ? child.subtreeFileCount : 1u;
                node.subtreeByteTotal += isDirectory ? child.subtreeByteTotal : child.size;
            }
            return node;
        }

        inline std::vector<std::string> parents(std::vector<SectionChildren> const& sections)
        {
            std::vector<std::string> result;
            for (auto const& section : sections)
                result.push_back(section.parentRelKey);
            return result;
        }
    }

    class SyncSessionTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            using namespace SyncSessionTestDetail;
            session_->setLocalTreeInStrand(directory(
                "",
                {
                    directory("new_module", {file("a.cpp", 1, 300), directory("src", {file("b.cpp", 2, 300)})}),
                    directory("src", {file("main.cpp", 21, 300), directory("util", {file("strings.cpp", 10, 300)})}),
                }
            ));
            session_->setRemoteTreeInStrand(directory(
                "",
                {
                    directory("src", {file("main.cpp", 12, 100), directory("util", {file("strings.cpp", 10, 100)})}),
                    directory("remote_tool", {file("tool", 9, 300)}),
                }
            ));
        }

        boost::asio::io_context context_{};
        std::shared_ptr<SyncSession> session_{std::make_shared<SyncSession>(
            SyncSession::Options{
                .sessionId = Ids::generateSyncSessionId(),
                .localRoot = Paths::LocalPath::fromUtf8("/local", Paths::LocalPlatform::Posix).value(),
                .remoteRoot = remote("/remote")
            },
            context_.get_executor()
        )};
    };

    TEST_F(SyncSessionTests, SnapshotCarriesRootsOfEverySection)
    {
        const auto snapshot = session_->recomputeSnapshotInStrand({}, {}, {});
        EXPECT_EQ(snapshot.summary.generation, 1u);
        ASSERT_EQ(snapshot.uploads.size(), 1u);
        EXPECT_EQ(snapshot.uploads[0].parentRelKey, "");
        ASSERT_EQ(snapshot.uploads[0].children.size(), 2u);
        EXPECT_EQ(snapshot.uploads[0].children[0].relKey, "new_module");
        EXPECT_EQ(snapshot.uploads[0].children[1].relKey, "src");
        EXPECT_TRUE(snapshot.uploads[0].children[1].isStructural);
        ASSERT_EQ(snapshot.downloads.size(), 1u);
        EXPECT_EQ(snapshot.downloads[0].children.size(), 1u);
        ASSERT_EQ(snapshot.deletes.size(), 1u);
        EXPECT_TRUE(snapshot.deletes[0].children.empty());
    }

    TEST_F(SyncSessionTests, SnapshotReturnsLoadedParentsParentsFirstAndDropsVanishedOnes)
    {
        using SyncSessionTestDetail::parents;
        const auto snapshot = session_->recomputeSnapshotInStrand(
            {}, {.uploads = {"src/util", "new_module/src", "gone", "src", "new_module"}}, {}
        );
        EXPECT_EQ(
            parents(snapshot.uploads), (std::vector<std::string>{"", "src", "src/util", "new_module", "new_module/src"})
        );
        const auto again = session_->recomputeSnapshotInStrand(
            {}, {.uploads = {"new_module", "src", "gone", "new_module/src", "src/util"}}, {}
        );
        EXPECT_EQ(again.uploads, snapshot.uploads) << "the parent order does not depend on the request order";
        auto const& lazy = snapshot.uploads[4];
        ASSERT_EQ(lazy.children.size(), 1u);
        EXPECT_EQ(lazy.children[0].relKey, "new_module/src/b.cpp");
        EXPECT_EQ(lazy.children[0].action, SharedData::Sync::Action::Upload);
    }

    TEST_F(SyncSessionTests, RecomputeWithOtherOptionsReplacesRowsAndBumpsGeneration)
    {
        session_->recomputeSnapshotInStrand({}, {}, {});
        const auto snapshot = session_->recomputeSnapshotInStrand(
            {.direction = SharedData::Sync::Direction::Upload, .actionDelete = true}, {.downloads = {"remote_tool"}}, {}
        );
        EXPECT_EQ(snapshot.summary.generation, 2u);
        EXPECT_TRUE(snapshot.downloads[0].children.empty());
        ASSERT_EQ(snapshot.downloads.size(), 1u) << "remote_tool is no download any more";
        ASSERT_EQ(snapshot.deletes[0].children.size(), 1u);
        EXPECT_EQ(snapshot.deletes[0].children[0].action, SharedData::Sync::Action::DeleteRemote);
    }

    TEST_F(SyncSessionTests, PlanResolvesBelowSessionRoots)
    {
        session_->recomputeSnapshotInStrand({}, {}, {});
        const auto plan = session_->resolvePlanInStrand(SharedData::Sync::DiffSection::Upload, {"src"});
        ASSERT_EQ(plan.entries.size(), 2u);
        EXPECT_TRUE(plan.refused.empty());
        EXPECT_EQ(plan.entries[0].entry.relKey, "src/main.cpp");
        EXPECT_EQ(plan.entries[0].paths.local.toUtf8(), "/local/src/main.cpp");
        EXPECT_EQ(plan.entries[0].paths.remote.value(), "/remote/src/main.cpp");
        EXPECT_EQ(plan.entries[1].entry.relKey, "src/util/strings.cpp");
    }

    TEST_F(SyncSessionTests, CancelOnlyStopsTheRunningWalk)
    {
        session_->cancel();
        EXPECT_TRUE(session_->isCancelled());
        const auto snapshot = session_->recomputeSnapshotInStrand({}, {}, {});
        EXPECT_FALSE(snapshot.summary.cancelled) << "a cancel before the walk does not leak into the next one";
        EXPECT_EQ(snapshot.summary.uploads.itemCount, 4u);
    }

    TEST_F(SyncSessionTests, RecomputeWithoutTreesReturnsAnEmptySummary)
    {
        boost::asio::io_context context;
        SyncSession empty{SyncSession::Options{}, context.get_executor()};
        const auto summary = empty.recomputeDiffInStrand({}, {});
        EXPECT_FALSE(empty.bothTreesReadyInStrand());
        EXPECT_EQ(summary.generation, 1u);
        EXPECT_EQ(summary.uploads.itemCount, 0u);
    }

    TEST_F(SyncSessionTests, RowsResolveBelowBothRootsOrNotAtAll)
    {
        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::fromUtf8("C:/sync", Paths::LocalPlatform::Windows).value(),
                .remoteRoot = remote("/srv/sync"),
            },
            context.get_executor()
        };
        const auto resolved = session.resolve(Paths::RelativePath::make("dir/a\\b").value());
        EXPECT_FALSE(resolved.has_value()) << "a backslash is a separator in a Windows path";

        const auto plain = session.resolve(Paths::RelativePath::make("dir/a.txt").value());
        ASSERT_TRUE(plain.has_value());
        EXPECT_EQ(plain->local.toUtf8(), "C:/sync/dir/a.txt");
        EXPECT_EQ(plain->remote.value(), "/srv/sync/dir/a.txt");

        EXPECT_FALSE(session.resolve(Paths::RelativePath::make("dir/CON").value()).has_value());
        EXPECT_EQ(session.resolve(Paths::RelativePath{})->remote.value(), "/srv/sync");
    }

    TEST(SyncSessionCaseTests, CaseVariantsBlockOnlyTowardsASideThatFoldsCase)
    {
        using namespace SyncSessionTestDetail;
        boost::asio::io_context context{};
        const auto blockedWith = [&](bool localFoldsCase, bool remoteFoldsCase, Direction direction)
        {
            SyncSession session{
                SyncSession::Options{
                    .localRoot = Paths::LocalPath::fromUtf8("/local", Paths::LocalPlatform::Posix).value(),
                    .remoteRoot = remote("/remote"),
                    .localFoldsCase = localFoldsCase,
                    .remoteFoldsCase = remoteFoldsCase,
                },
                context.get_executor()
            };
            session.setLocalTreeInStrand(directory("", {file("Readme", 1, 200)}));
            session.setRemoteTreeInStrand(directory("", {file("readme", 1, 100)}));
            return session.recomputeDiffInStrand({.direction = direction}, {}).blockedCount;
        };
        EXPECT_EQ(blockedWith(false, false, Direction::Both), 0u);
        EXPECT_EQ(blockedWith(true, false, Direction::Upload), 0u);
        EXPECT_EQ(blockedWith(true, false, Direction::Download), 2u);
        EXPECT_EQ(blockedWith(false, true, Direction::Upload), 2u);
        EXPECT_EQ(blockedWith(false, true, Direction::Download), 0u);
    }
}
