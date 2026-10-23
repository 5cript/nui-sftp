#pragma once

#include <backend/file_tracking/tracking_state.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Test::TrackingState
{
    using namespace FileTracking;

    namespace TrackingStateTestDetail
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

    using namespace TrackingStateTestDetail;

    TEST(TrackingStateTests, UnchangedServerCopyIsNoChange)
    {
        EXPECT_FALSE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 5, .size = 10}));
    }

    TEST(TrackingStateTests, OtherTimeOrSizeIsAChange)
    {
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 6, .size = 10}));
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, ServerState{.mtime = 5, .size = 11}));
    }

    TEST(TrackingStateTests, VanishedServerCopyIsAChange)
    {
        EXPECT_TRUE(serverChanged(ServerState{.mtime = 5, .size = 10}, std::nullopt));
    }

    TEST(TrackingStateTests, UnseenServerFileIsAChangeAndAMissingOneIsNot)
    {
        EXPECT_TRUE(serverChanged(std::nullopt, ServerState{.mtime = 5, .size = 10}));
        EXPECT_FALSE(serverChanged(std::nullopt, std::nullopt));
    }

    TEST(TrackingStateTests, ChangeMapsBelowTheRemoteItem)
    {
        const auto mapped = remotePathOfChange(
            local("/tmp/instance/project/src/main.cpp"), local("/tmp/instance/project"), remote("/home/user/project")
        );
        ASSERT_TRUE(mapped.has_value());
        EXPECT_EQ(mapped->value(), "/home/user/project/src/main.cpp");
    }

    TEST(TrackingStateTests, ChangeOfTheItemMapsToItsRemotePath)
    {
        const auto mapped =
            remotePathOfChange(local("/tmp/instance/file.txt"), local("/tmp/instance/file.txt"), remote("/home/user/file.txt"));
        ASSERT_TRUE(mapped.has_value());
        EXPECT_EQ(mapped->value(), "/home/user/file.txt");
    }

    TEST(TrackingStateTests, ChangeOutsideTheItemMapsToNothing)
    {
        const auto mapped =
            remotePathOfChange(local("/tmp/instance/other.txt"), local("/tmp/instance/file.txt"), remote("/home/user/file.txt"));
        ASSERT_FALSE(mapped.has_value());
        EXPECT_EQ(mapped.error().reason, Paths::PathRejection::EscapesRoot);
    }

    TEST(TrackingStateTests, AnItemNamedOtherwiseLocallyStillMapsToItsRemotePath)
    {
        const auto mapped =
            remotePathOfChange(local("/home/me/renamed/a.txt"), local("/home/me/renamed"), remote("/srv/original"));
        ASSERT_TRUE(mapped.has_value());
        EXPECT_EQ(mapped->value(), "/srv/original/a.txt");
    }

    TEST(TrackingStateTests, MoveMeanings)
    {
        EXPECT_EQ(meaningOfMove(false, true), MoveMeaning::Rename);
        EXPECT_EQ(meaningOfMove(true, true), MoveMeaning::ReplaceDestination);
        EXPECT_EQ(meaningOfMove(true, false), MoveMeaning::ReplaceDestination);
        EXPECT_EQ(meaningOfMove(false, false), MoveMeaning::ReplaceDestination);
    }

    TEST(TrackingStateTests, KnownContentFindsFilesAndWhatLiesBelowDirectories)
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

    TEST(TrackingStateTests, KnownContentForgetsATree)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir/a.txt"), ServerState{});
        known.rememberDirectory(local("/i/dir/sub"));
        known.rememberFile(local("/i/other.txt"), ServerState{});
        known.forget(local("/i/dir"));
        EXPECT_FALSE(known.has(local("/i/dir")));
        EXPECT_TRUE(known.has(local("/i/other.txt")));
    }

    TEST(TrackingStateTests, KnownContentMovesATree)
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

    TEST(TrackingStateTests, KnownContentTellsPathsBelowFromSiblingsThatSortBetween)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir.txt"), ServerState{});
        known.rememberFile(local("/i/dir-old/a.txt"), ServerState{});
        known.rememberDirectory(local("/i/dir"));
        EXPECT_FALSE(known.hasBelow(local("/i/dir")));
        known.rememberFile(local("/i/dir/z.txt"), ServerState{});
        EXPECT_TRUE(known.hasBelow(local("/i/dir")));
        EXPECT_TRUE(known.hasBelow(local("/")));
        EXPECT_FALSE(known.hasBelow(local("/i/dir.txt")));

        known.forget(local("/i/dir"));
        EXPECT_FALSE(known.has(local("/i/dir")));
        EXPECT_TRUE(known.has(local("/i/dir.txt")));
        EXPECT_TRUE(known.has(local("/i/dir-old/a.txt")));
    }

    TEST(TrackingStateTests, KnownContentMovedOntoKnownPathsReplacesThem)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/a.txt"), ServerState{.mtime = 1, .size = 1});
        known.rememberFile(local("/i/b.txt"), ServerState{.mtime = 2, .size = 2});
        known.move(local("/i/a.txt"), local("/i/b.txt"));
        EXPECT_FALSE(known.has(local("/i/a.txt")));
        EXPECT_EQ(known.fileState(local("/i/b.txt")), (ServerState{.mtime = 1, .size = 1}));
    }

#ifdef __APPLE__
    TEST(TrackingStateTests, KnownContentFindsTheSpellingItKnowsInAnotherNormalization)
    {
        KnownServerContent known;
        known.rememberFile(local("/w/uni/cafe\xCC\x81.txt"), ServerState{.mtime = 1, .size = 1});
        known.rememberDirectory(local("/w/r\xC3\xA9sum\xC3\xA9s"));
        known.rememberFile(local("/w/plain.txt"), ServerState{.mtime = 1, .size = 1});

        EXPECT_EQ(known.knownSpellingOf(local("/w/uni/caf\xC3\xA9.txt")), local("/w/uni/cafe\xCC\x81.txt"));
        EXPECT_EQ(known.knownSpellingOf(local("/w/uni/cafe\xCC\x81.txt")), local("/w/uni/cafe\xCC\x81.txt"));
        EXPECT_EQ(known.knownSpellingOf(local("/w/re\xCC\x81sume\xCC\x81s")), local("/w/r\xC3\xA9sum\xC3\xA9s"));
        EXPECT_EQ(known.knownSpellingOf(local("/w/plain.txt")), local("/w/plain.txt"));
        EXPECT_FALSE(known.knownSpellingOf(local("/w/uni/CAF\xC3\x89.txt")).has_value()) << "case stays apart";
        EXPECT_FALSE(known.knownSpellingOf(local("/w/other.txt")).has_value());
    }

    TEST(TrackingStateTests, KnownContentFindsTheSpellingInEveryScript)
    {
        KnownServerContent known;
        // Decomposed like Finder writes them: a kana with the voiced sound mark, a Hangul syllable as its jamo and a
        // Vietnamese letter with two marks.
        known.rememberFile(local("/w/\xE3\x81\x8B\xE3\x82\x99.txt"), ServerState{.mtime = 1, .size = 1});
        known.rememberFile(local("/w/\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB.txt"), ServerState{.mtime = 1, .size = 1});
        known.rememberDirectory(local("/w/vie\xCC\xA3\xCC\x82t"));
        known.rememberFile(local("/w/cafe\xCC\x81/notes.txt"), ServerState{.mtime = 1, .size = 1});

        EXPECT_EQ(known.knownSpellingOf(local("/w/\xE3\x81\x8C.txt")), local("/w/\xE3\x81\x8B\xE3\x82\x99.txt"));
        EXPECT_EQ(
            known.knownSpellingOf(local("/w/\xED\x95\x9C.txt")), local("/w/\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB.txt")
        );
        EXPECT_EQ(known.knownSpellingOf(local("/w/vi\xE1\xBB\x87t")), local("/w/vie\xCC\xA3\xCC\x82t"));
        EXPECT_EQ(known.knownSpellingOf(local("/w/vie\xCC\x82\xCC\xA3t")), local("/w/vie\xCC\xA3\xCC\x82t"))
            << "the marks in another order";
        EXPECT_EQ(known.knownSpellingOf(local("/w/caf\xC3\xA9/notes.txt")), local("/w/cafe\xCC\x81/notes.txt"));
    }

    TEST(TrackingStateTests, KnownContentFindsNoSpellingWhenSeveralMatch)
    {
        KnownServerContent known;
        known.rememberFile(local("/w/r\xC3\xA9sum\xC3\xA9"), ServerState{.mtime = 1, .size = 1});
        known.rememberFile(local("/w/re\xCC\x81sume\xCC\x81"), ServerState{.mtime = 2, .size = 2});

        EXPECT_FALSE(known.knownSpellingOf(local("/w/r\xC3\xA9sume\xCC\x81")).has_value());
        EXPECT_EQ(known.knownSpellingOf(local("/w/r\xC3\xA9sum\xC3\xA9")), local("/w/r\xC3\xA9sum\xC3\xA9"));
    }
#else
    TEST(TrackingStateTests, KnownContentKeepsSpellingsInAnotherNormalizationApart)
    {
        KnownServerContent known;
        known.rememberFile(local("/w/uni/cafe\xCC\x81.txt"), ServerState{.mtime = 1, .size = 1});

        EXPECT_EQ(known.knownSpellingOf(local("/w/uni/cafe\xCC\x81.txt")), local("/w/uni/cafe\xCC\x81.txt"));
        EXPECT_FALSE(known.knownSpellingOf(local("/w/uni/caf\xC3\xA9.txt")).has_value()) << "ext4 and NTFS hold both";
    }
#endif

    TEST(TrackingStateTests, UploadsBelowAMovedDirectoryReadTheNewPaths)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/dir/a.txt")}));
        ASSERT_TRUE(pending.start({.path = local("/i/other.txt")}));
        EXPECT_FALSE(pending.movedAway(local("/i/dir/a.txt")));

        pending.moveBelow(local("/i/dir"), local("/i/renamed"), /*serverCopiesFollow=*/true);

        EXPECT_TRUE(pending.movedAway(local("/i/dir/a.txt")));
        EXPECT_FALSE(pending.movedAway(local("/i/other.txt")));
        EXPECT_TRUE(pending.isPending(local("/i/renamed/a.txt")));
        const auto finished = pending.finish(local("/i/dir/a.txt"));
        EXPECT_EQ(finished.movedTo, local("/i/renamed/a.txt"));
        EXPECT_EQ(finished.serverCopyAt, local("/i/renamed/a.txt"));
    }

    TEST(TrackingStateTests, ServerCopiesStayWhenTheServerDirectoryWasNotRenamed)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/dir/a.txt")}));
        pending.moveBelow(local("/i/dir"), local("/i/renamed"), /*serverCopiesFollow=*/false);
        const auto finished = pending.finish(local("/i/dir/a.txt"));
        EXPECT_EQ(finished.movedTo, local("/i/renamed/a.txt"));
        EXPECT_FALSE(finished.serverCopyAt.has_value());
    }

    TEST(TrackingStateTests, ChangesWaitingBelowADirectoryFollowAMoveBelowIt)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/item"), .isDirectory = true}));
        ASSERT_FALSE(pending.start({.path = local("/i/item/sub/a.txt")}));
        pending.moveBelow(local("/i/item/sub"), local("/i/item/moved"), /*serverCopiesFollow=*/true);
        EXPECT_FALSE(pending.movedAway(local("/i/item")));
        EXPECT_EQ(
            pending.finish(local("/i/item")).changedBelow,
            (std::vector<LocalChange>{{.path = local("/i/item/moved/a.txt")}})
        );
    }

    TEST(TrackingStateTests, SecondChangeOfAPendingUploadGoesAgainAfterIt)
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

    TEST(TrackingStateTests, ChangesBeforeTheUploadReadsTheFileNeedNothingMore)
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

    TEST(TrackingStateTests, ChangesHandledKeepsAPendingRemoval)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.removeRemoteWhenDone(local("/i/a.txt")));
        pending.changesHandled(local("/i/a.txt"));
        EXPECT_TRUE(pending.finish(local("/i/a.txt")).removeRemote);
    }

    TEST(TrackingStateTests, UploadWithoutFurtherChangeEndsQuietly)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        const auto finished = pending.finish(local("/i/a.txt"));
        EXPECT_FALSE(finished.changedAgain);
        EXPECT_TRUE(finished.changedBelow.empty());
    }

    TEST(TrackingStateTests, ChangesBelowAPendingDirectoryWaitForIt)
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

    TEST(TrackingStateTests, APendingUploadMovedAwayTellsWhereItWentWhenItEnds)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/dir"), .isDirectory = true}));
        ASSERT_FALSE(pending.start({.path = local("/i/dir/a.txt")}));
        EXPECT_FALSE(pending.moveWhenDone(local("/i/other"), local("/i/elsewhere")));
        ASSERT_TRUE(pending.moveWhenDone(local("/i/dir"), local("/i/renamed")));
        ASSERT_TRUE(pending.moveWhenDone(local("/i/renamed"), local("/i/final")));

        EXPECT_FALSE(pending.isPending(local("/i/dir")));
        EXPECT_TRUE(pending.isPending(local("/i/final")));
        EXPECT_FALSE(pending.start({.path = local("/i/final/b.txt")}));
        EXPECT_TRUE(pending.start({.path = local("/i/dir/c.txt")}));

        const auto finished = pending.finish(local("/i/dir"));
        EXPECT_EQ(finished.movedTo, local("/i/final"));
        EXPECT_FALSE(finished.changedAgain);
        EXPECT_EQ(
            finished.changedBelow,
            (std::vector<LocalChange>{{.path = local("/i/final/a.txt")}, {.path = local("/i/final/b.txt")}})
        );
    }

    TEST(TrackingStateTests, ChangesAtWhereAPendingUploadMovedGoAgainAfterIt)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.moveWhenDone(local("/i/a.txt"), local("/i/b.txt")));
        EXPECT_FALSE(pending.start({.path = local("/i/b.txt")}));
        EXPECT_TRUE(pending.finish(local("/i/a.txt")).changedAgain);

        ASSERT_TRUE(pending.start({.path = local("/i/c.txt")}));
        ASSERT_TRUE(pending.moveWhenDone(local("/i/c.txt"), local("/i/d.txt")));
        EXPECT_TRUE(pending.removeRemoteWhenDone(local("/i/d.txt")));
        EXPECT_TRUE(pending.finish(local("/i/c.txt")).removeRemote);
    }

    TEST(TrackingStateTests, FilesNextToAPendingFileStartTheirOwn)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        EXPECT_TRUE(pending.start({.path = local("/i/a.txt.bak")}));
    }

    TEST(TrackingStateTests, PendingUploadMovedAwayRemovesItsRemoteCopyInsteadOfGoingAgain)
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

    TEST(TrackingStateTests, ChangeAfterRemovalGoesAgainAndKeepsTheRemoteCopy)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.removeRemoteWhenDone(local("/i/a.txt")));
        ASSERT_FALSE(pending.start({.path = local("/i/a.txt")}));
        const auto finished = pending.finish(local("/i/a.txt"));
        EXPECT_TRUE(finished.changedAgain);
        EXPECT_FALSE(finished.removeRemote);
    }

    TEST(TrackingStateTests, KeptDirectoriesAreForgottenAndMovedWithTheirTree)
    {
        KnownServerContent known;
        known.rememberFile(local("/i/dir/sub/a.txt"), ServerState{});
        known.rememberDirectory(local("/i/dir/sub"));
        known.keep(local("/i/dir/sub"));
        known.keep(local("/i/dir"));
        EXPECT_EQ(known.keptDirectories(), (std::vector{local("/i/dir"), local("/i/dir/sub")}));

        known.move(local("/i/dir"), local("/i/moved"));
        EXPECT_EQ(known.keptDirectories(), (std::vector{local("/i/moved"), local("/i/moved/sub")}));
        known.forget(local("/i/moved/sub"));
        EXPECT_EQ(known.keptDirectories(), std::vector{local("/i/moved")});
    }

    TEST(TrackingStateTests, AKeptDirectoryIsEmptiedOnceNothingIsKnownBelowIt)
    {
        KnownServerContent known;
        known.rememberDirectory(local("/i/new"));
        known.rememberDirectory(local("/i/new/deep"));
        known.rememberFile(local("/i/new/deep/n0.txt"), ServerState{});
        known.rememberFile(local("/i/new/deep/n1.txt"), ServerState{});
        known.keep(local("/i/new"));
        known.keep(local("/i/new/deep"));

        known.forget(local("/i/new/deep/n0.txt"));
        EXPECT_TRUE(known.emptiedKeptAbove(local("/i/new/deep/n0.txt")).empty());
        known.forget(local("/i/new/deep/n1.txt"));
        EXPECT_EQ(known.emptiedKeptAbove(local("/i/new/deep/n1.txt")), std::vector{local("/i/new/deep")});
        known.forget(local("/i/new/deep"));
        EXPECT_EQ(known.emptiedKeptAbove(local("/i/new/deep")), std::vector{local("/i/new")});
    }

    TEST(TrackingStateTests, ACopyHoldsAllBelowWhenEveryKnownEntryIsThereAlike)
    {
        KnownServerContent known;
        for (auto const* base : {"/i/src/x", "/i/docs/x"})
        {
            known.rememberDirectory(local(std::string{base} + "/y"));
            known.rememberFile(local(std::string{base} + "/main.c"), ServerState{.mtime = 5, .size = 6});
            known.rememberFile(local(std::string{base} + "/y/deep.txt"), ServerState{.mtime = 7, .size = 8});
        }
        known.rememberFile(local("/i/docs/x/added.txt"), ServerState{});
        known.rememberDirectory(local("/i/empty"));

        EXPECT_TRUE(known.holdsAllBelow(local("/i/docs/x"), local("/i/src/x")));
        EXPECT_FALSE(known.holdsAllBelow(local("/i/src/x"), local("/i/docs/x")));
        EXPECT_FALSE(known.holdsAllBelow(local("/i/docs/x"), local("/i/empty")));
        known.rememberFile(local("/i/docs/x/y/deep.txt"), ServerState{.mtime = 9, .size = 8});
        EXPECT_FALSE(known.holdsAllBelow(local("/i/docs/x"), local("/i/src/x")));
    }

    TEST(TrackingStateTests, ATreeIsRemovedDeepestFirst)
    {
        KnownServerContent known;
        known.rememberDirectory(local("/i/x"));
        known.rememberDirectory(local("/i/x/y"));
        known.rememberFile(local("/i/x/y/deep.txt"), ServerState{});
        known.rememberFile(local("/i/x/main.c"), ServerState{});
        known.rememberFile(local("/i/x-other.txt"), ServerState{});

        EXPECT_EQ(
            known.deepestFirst(local("/i/x")),
            (std::vector{local("/i/x/y/deep.txt"), local("/i/x/y"), local("/i/x/main.c"), local("/i/x")})
        );
        EXPECT_EQ(known.deepestFirst(local("/i/x-other.txt")), std::vector{local("/i/x-other.txt")});
    }

    TEST(TrackingStateTests, APendingUploadTellsTheKindItsPathHasAtTheLatestChange)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/kind"), .isDirectory = false}));
        EXPECT_FALSE(pending.start({.path = local("/i/kind"), .isDirectory = true}));
        EXPECT_FALSE(pending.start({.path = local("/i/kind/inside.txt")}));
        const auto finished = pending.finish(local("/i/kind"));
        EXPECT_TRUE(finished.changedAgain);
        EXPECT_TRUE(finished.isDirectory);
        EXPECT_EQ(finished.changedBelow, std::vector<LocalChange>{{.path = local("/i/kind/inside.txt")}});
    }

    TEST(TrackingStateTests, AnOperationWaitsForTheEarlierOnesOnItsPathAboveAndBelow)
    {
        ServerOperationOrder order;
        const auto [deleteFile, deleteFileStarts] = order.enter({remote("/r/dir/a.txt")});
        const auto [deleteSibling, deleteSiblingStarts] = order.enter({remote("/r/dir/b.txt")});
        const auto [deleteDirectory, deleteDirectoryStarts] = order.enter({remote("/r/dir")});
        const auto [upload, uploadStarts] = order.enter({remote("/r/dir/a.txt")});
        const auto [elsewhere, elsewhereStarts] = order.enter({remote("/r/other")});

        EXPECT_TRUE(deleteFileStarts);
        EXPECT_TRUE(deleteSiblingStarts);
        EXPECT_FALSE(deleteDirectoryStarts);
        EXPECT_FALSE(uploadStarts);
        EXPECT_TRUE(elsewhereStarts);

        EXPECT_TRUE(order.leave(deleteFile).empty());
        EXPECT_EQ(order.leave(deleteSibling), std::vector{deleteDirectory});
        EXPECT_EQ(order.leave(deleteDirectory), std::vector{upload});
        EXPECT_TRUE(order.leave(upload).empty());
        EXPECT_TRUE(order.leave(elsewhere).empty());
        EXPECT_TRUE(order.empty());
    }

    TEST(TrackingStateTests, ARenameWaitsOnBothOfItsPaths)
    {
        ServerOperationOrder order;
        const auto [upload, uploadStarts] = order.enter({remote("/r/b.txt")});
        const auto [rename, renameStarts] = order.enter({remote("/r/a.txt"), remote("/r/b.txt")});
        const auto [uploadOfSource, uploadOfSourceStarts] = order.enter({remote("/r/a.txt")});

        EXPECT_TRUE(uploadStarts);
        EXPECT_FALSE(renameStarts);
        EXPECT_FALSE(uploadOfSourceStarts);
        EXPECT_EQ(order.leave(upload), std::vector{rename});
        EXPECT_EQ(order.leave(rename), std::vector{uploadOfSource});
    }

    TEST(TrackingStateTests, LeavingAnUnknownTicketStartsNothing)
    {
        ServerOperationOrder order;
        const auto [first, firstStarts] = order.enter({remote("/r/a.txt")});
        const auto [second, secondStarts] = order.enter({remote("/r/a.txt")});
        EXPECT_FALSE(secondStarts);
        EXPECT_TRUE(order.leave(second + 1u).empty());
        EXPECT_EQ(order.leave(first), std::vector{second});
    }

    TEST(TrackingStateTests, KnownContentRemembersTheStateAKnownFileWasSentIn)
    {
        KnownServerContent known;
        const SourceState sent{.size = 4, .modified = std::filesystem::file_time_type{std::chrono::seconds{7}}};
        known.rememberSent(local("/i/unknown.txt"), sent);
        known.rememberFile(local("/i/a.txt"), ServerState{.mtime = 7, .size = 4});
        known.rememberSent(local("/i/a.txt"), sent);

        EXPECT_FALSE(known.sentFrom(local("/i/unknown.txt")).has_value());
        EXPECT_EQ(known.sentFrom(local("/i/a.txt")), sent);
        known.rememberFile(local("/i/a.txt"), ServerState{.mtime = 8, .size = 4});
        EXPECT_FALSE(known.sentFrom(local("/i/a.txt")).has_value());
    }

    TEST(TrackingStateTests, KnownContentForgetsAndMovesWhatWasSentWithItsTree)
    {
        KnownServerContent known;
        const SourceState sent{.size = 4, .modified = std::filesystem::file_time_type{std::chrono::seconds{7}}};
        known.rememberFile(local("/i/dir/a.txt"), ServerState{});
        known.rememberSent(local("/i/dir/a.txt"), sent);
        known.rememberFile(local("/i/gone/b.txt"), ServerState{});
        known.rememberSent(local("/i/gone/b.txt"), sent);

        known.move(local("/i/dir"), local("/i/renamed"));
        known.forget(local("/i/gone"));

        EXPECT_FALSE(known.sentFrom(local("/i/dir/a.txt")).has_value());
        EXPECT_EQ(known.sentFrom(local("/i/renamed/a.txt")), sent);
        EXPECT_FALSE(known.sentFrom(local("/i/gone/b.txt")).has_value());
    }

    TEST(TrackingStateTests, AnUploadCoversItsPathAndAPendingDirectoryWhatLiesBelowIt)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/new"), .isDirectory = true}));
        ASSERT_TRUE(pending.start({.path = local("/i/file.txt")}));

        EXPECT_TRUE(pending.covers(local("/i/new")));
        EXPECT_TRUE(pending.covers(local("/i/new/sub/a.txt")));
        EXPECT_TRUE(pending.covers(local("/i/file.txt")));
        EXPECT_FALSE(pending.covers(local("/i/file.txt/below")));
        EXPECT_FALSE(pending.covers(local("/i/newer.txt")));
    }

    TEST(TrackingStateTests, AnUploadCountsTheEntriesItCarriesAndLateChangesBelowItOnlyBeyondThem)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/new"), .isDirectory = true}));
        EXPECT_FALSE(pending.start({.path = local("/i/new/a.txt")}));
        EXPECT_EQ(pending.changeCount(), 2u);

        pending.changesHandled(local("/i/new"));
        EXPECT_EQ(pending.changeCount(), 2u);
        pending.carries(local("/i/new"), 4u);
        EXPECT_EQ(pending.changeCount(), 4u);
        EXPECT_FALSE(pending.start({.path = local("/i/new/a.txt")}));
        EXPECT_FALSE(pending.start({.path = local("/i/new/b.txt")}));
        EXPECT_EQ(pending.changeCount(), 4u);
        EXPECT_FALSE(pending.start({.path = local("/i/new"), .isDirectory = true}));
        EXPECT_EQ(pending.changeCount(), 4u);

        EXPECT_EQ(pending.finish(local("/i/new")).counted, 4u);
        EXPECT_EQ(pending.changeCount(), 0u);
    }

    TEST(TrackingStateTests, AFileThatChangedAgainCountsOnceMoreADirectoryDoesNot)
    {
        PendingUploads pending;
        ASSERT_TRUE(pending.start({.path = local("/i/a.txt")}));
        ASSERT_TRUE(pending.start({.path = local("/i/new"), .isDirectory = true}));
        pending.carries(local("/i/new"), 3u);
        ASSERT_EQ(pending.changeCount(), 4u);

        EXPECT_FALSE(pending.start({.path = local("/i/a.txt")}));
        EXPECT_FALSE(pending.start({.path = local("/i/new"), .isDirectory = true}));

        EXPECT_EQ(pending.changeCount(), 5u);
    }
}
