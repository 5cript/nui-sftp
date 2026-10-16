#pragma once

#include <frontend/session_components/file_tracking_state.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace FileTracking::Test
{
    namespace FileTrackingStateTestDetail
    {
        inline Paths::LocalPath local(std::string_view path)
        {
            return Paths::LocalPath::fromUtf8(path, Paths::LocalPlatform::Posix).value();
        }

        inline Paths::RemotePath remote(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }
    }

    using namespace FileTrackingStateTestDetail;

    TEST(FileTrackingStateTests, UnchangedServerCopyIsNoChange)
    {
        EXPECT_FALSE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 5, .size = 10}));
    }

    TEST(FileTrackingStateTests, OtherTimeOrSizeIsAChange)
    {
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 6, .size = 10}));
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 5, .size = 11}));
    }

    TEST(FileTrackingStateTests, VanishedServerCopyIsAChange)
    {
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, std::nullopt));
    }

    TEST(FileTrackingStateTests, UnseenServerFileIsAChangeAndAMissingOneIsNot)
    {
        EXPECT_TRUE(serverChanged(std::nullopt, ServerState{.mtime = 5, .size = 10}));
        EXPECT_FALSE(serverChanged(std::nullopt, std::nullopt));
    }

    TEST(FileTrackingStateTests, ChangeMapsBelowTheRemoteDirectory)
    {
        const auto mapped = remotePathOfChange(
            local("/tmp/instance/project/src/main.cpp"), local("/tmp/instance"), remote("/home/user/project")
        );
        ASSERT_TRUE(mapped.has_value());
        EXPECT_EQ(mapped->value(), "/home/user/project/src/main.cpp");
    }

    TEST(FileTrackingStateTests, ChangeOfTheInstanceDirectoryMapsToNothing)
    {
        const auto mapped =
            remotePathOfChange(local("/tmp/instance"), local("/tmp/instance"), remote("/home/user/file.txt"));
        ASSERT_FALSE(mapped.has_value());
        EXPECT_EQ(mapped.error().reason, Paths::PathRejection::Empty);
    }

    TEST(FileTrackingStateTests, ChangeOutsideTheInstanceMapsToNothing)
    {
        const auto mapped =
            remotePathOfChange(local("/tmp/other/file.txt"), local("/tmp/instance"), remote("/home/user/file.txt"));
        ASSERT_FALSE(mapped.has_value());
        EXPECT_EQ(mapped.error().reason, Paths::PathRejection::EscapesRoot);
    }

    TEST(FileTrackingStateTests, TrackedRootHasNoRemoteDirectory)
    {
        EXPECT_FALSE(remotePathOfChange(local("/tmp/instance/x"), local("/tmp/instance"), Paths::RemotePath::root())
                         .has_value());
    }

    TEST(FileTrackingStateTests, MoveMeanings)
    {
        EXPECT_EQ(meaningOfMove(false, true), MoveMeaning::Rename);
        EXPECT_EQ(meaningOfMove(true, true), MoveMeaning::ReplaceDestination);
        EXPECT_EQ(meaningOfMove(true, false), MoveMeaning::ReplaceDestination);
        EXPECT_EQ(meaningOfMove(false, false), MoveMeaning::ReplaceDestination);
    }

    TEST(FileTrackingStateTests, KnownContentFindsFilesAndWhatLiesBelowDirectories)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir/a.txt"), ServerState{.mtime = 1, .size = 2});
        known.rememberDirectory(local("/i/empty"));
        EXPECT_EQ(known.fileState(local("/i/dir/a.txt")), (ServerState{.mtime = 1, .size = 2}));
        EXPECT_FALSE(known.fileState(local("/i/dir")).has_value());
        EXPECT_TRUE(known.has(local("/i/dir")));
        EXPECT_TRUE(known.hasBelow(local("/i/dir")));
        EXPECT_FALSE(known.hasBelow(local("/i/dir/a.txt")));
        EXPECT_TRUE(known.has(local("/i/empty")));
        EXPECT_FALSE(known.hasBelow(local("/i/empty")));
        EXPECT_FALSE(known.has(local("/i/di")));
    }

    TEST(FileTrackingStateTests, KnownContentForgetsATree)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir/a.txt"), ServerState{});
        known.rememberDirectory(local("/i/dir/sub"));
        known.rememberFile(local("/i/other.txt"), ServerState{});
        known.forget(local("/i/dir"));
        EXPECT_FALSE(known.has(local("/i/dir")));
        EXPECT_TRUE(known.has(local("/i/other.txt")));
    }

    TEST(FileTrackingStateTests, KnownContentMovesATree)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir/a.txt"), ServerState{.mtime = 3, .size = 4});
        known.rememberDirectory(local("/i/dir/sub"));
        known.rememberFile(local("/i/dirt.txt"), ServerState{});
        known.move(local("/i/dir"), local("/i/renamed"));
        EXPECT_FALSE(known.has(local("/i/dir")));
        EXPECT_EQ(known.fileState(local("/i/renamed/a.txt")), (ServerState{.mtime = 3, .size = 4}));
        EXPECT_TRUE(known.has(local("/i/renamed/sub")));
        EXPECT_TRUE(known.has(local("/i/dirt.txt")));
    }

    TEST(FileTrackingStateTests, SecondChangeOfAPendingUploadGoesAgainAfterIt)
    {
        PendingUploads pending;
        EXPECT_TRUE(pending.start({.path = local("/i/a.txt")}));
        EXPECT_FALSE(pending.start({.path = local("/i/a.txt")}));
        EXPECT_TRUE(pending.isPending(local("/i/a.txt")));
        const auto finished = pending.finish(local("/i/a.txt"));
        EXPECT_TRUE(finished.changedAgain);
        EXPECT_FALSE(finished.removeRemote);
        EXPECT_TRUE(pending.empty());
    }

    TEST(FileTrackingStateTests, ChangesBeforeTheUploadReadsTheFileNeedNothingMore)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_FALSE(pending.start({.path = local("/i/a.txt")}));
        pending.changesHandled(local("/i/a.txt"));
        EXPECT_FALSE(pending.finish(local("/i/a.txt")).changedAgain);

        ASSERT_TRUE(pending.start({.path = local("/i/dir"), .isDirectory = true}));
        ASSERT_FALSE(pending.start({.path = local("/i/dir/b.txt")}));
        pending.changesHandled(local("/i/dir"));
        ASSERT_FALSE(pending.start({.path = local("/i/dir/c.txt")}));
        EXPECT_EQ(pending.finish(local("/i/dir")).changedBelow, (std::vector<LocalChange>{{.path = local("/i/dir/c.txt")}}));
    }

    TEST(FileTrackingStateTests, ChangesHandledKeepsAPendingRemoval)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.removeRemoteWhenDone(local("/i/a.txt")));
        pending.changesHandled(local("/i/a.txt"));
        EXPECT_TRUE(pending.finish(local("/i/a.txt")).removeRemote);
    }

    TEST(FileTrackingStateTests, UploadWithoutFurtherChangeEndsQuietly)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        const auto finished = pending.finish(local("/i/a.txt"));
        EXPECT_FALSE(finished.changedAgain);
        EXPECT_TRUE(finished.changedBelow.empty());
    }

    TEST(FileTrackingStateTests, ChangesBelowAPendingDirectoryWaitForIt)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/new"), .isDirectory = true}));
        EXPECT_FALSE(pending.start({.path = local("/i/new/a.txt")}));
        EXPECT_FALSE(pending.start({.path = local("/i/new/sub"), .isDirectory = true}));
        EXPECT_FALSE(pending.start({.path = local("/i/new/a.txt")}));
        EXPECT_FALSE(pending.isPending(local("/i/new/a.txt")));
        EXPECT_TRUE(pending.start({.path = local("/i/newer.txt")}));

        const auto finished = pending.finish(local("/i/new"));
        EXPECT_EQ(
            finished.changedBelow,
            (std::vector<LocalChange>{{.path = local("/i/new/a.txt")}, {.path = local("/i/new/sub"), .isDirectory = true}})
        );
        EXPECT_TRUE(pending.start({.path = local("/i/new/a.txt")}));
    }

    TEST(FileTrackingStateTests, FilesNextToAPendingFileStartTheirOwn)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        EXPECT_TRUE(pending.start({.path = local("/i/a.txt.bak")}));
    }

    TEST(FileTrackingStateTests, PendingUploadMovedAwayRemovesItsRemoteCopyInsteadOfGoingAgain)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.tmp")}));
        ASSERT_FALSE(pending.start({.path = local("/i/a.tmp")}));
        EXPECT_TRUE(pending.removeRemoteWhenDone(local("/i/a.tmp")));
        EXPECT_FALSE(pending.removeRemoteWhenDone(local("/i/b.tmp")));
        const auto finished = pending.finish(local("/i/a.tmp"));
        EXPECT_TRUE(finished.removeRemote);
        EXPECT_FALSE(finished.changedAgain);
    }

    TEST(FileTrackingStateTests, ChangeAfterRemovalGoesAgainAndKeepsTheRemoteCopy)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.removeRemoteWhenDone(local("/i/a.txt")));
        ASSERT_FALSE(pending.start({.path = local("/i/a.txt")}));
        const auto finished = pending.finish(local("/i/a.txt"));
        EXPECT_TRUE(finished.changedAgain);
        EXPECT_FALSE(finished.removeRemote);
    }

    TEST(FileTrackingStateTests, RowOrderGivesTheIndexOfARowWhenItIsRemoved)
    {
        RowOrder<std::string> rows;
        rows.add("first");
        rows.add("second");
        rows.add("third");

        EXPECT_EQ(rows.remove("second"), std::size_t{1});
        EXPECT_EQ(rows.remove("third"), std::size_t{1});
        EXPECT_EQ(rows.remove("first"), std::size_t{0});
        EXPECT_EQ(rows.size(), std::size_t{0});
    }

    TEST(FileTrackingStateTests, RowOrderKnowsNoRowOfAnUnknownOrRemovedKey)
    {
        RowOrder<std::string> rows;
        rows.add("only");

        EXPECT_FALSE(rows.remove("other").has_value());
        ASSERT_TRUE(rows.remove("only").has_value());
        EXPECT_FALSE(rows.remove("only").has_value());
    }
}
