#pragma once

#include <utility/link_safe_remove.hpp>
#include <utility/no_follow_tree_walk.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifndef _WIN32
#    include <unistd.h>
#endif

extern std::filesystem::path programDirectory;
extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class LinkSafeRemoveTests : public ::testing::Test
    {
      protected:
        /**
         * @brief A link at @p link to the directory @p target: a junction on Windows, a symlink elsewhere.
         */
        static bool makeDirectoryLink(std::filesystem::path const& link, std::filesystem::path const& target)
        {
#ifdef _WIN32
            const auto command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
            return std::system(command.c_str()) == 0 && std::filesystem::exists(link);
#else
            std::error_code error;
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
#endif
        }

        /**
         * @brief A tree with content, a nested directory link out of it and a file link: tree/a.txt (1 byte),
         *        tree/sub/b.txt (2 bytes), tree/sub/deeper/c.txt (3 bytes), tree/sub/deeper/out -> outside and
         *        tree/fileLink -> outside/keep.txt. 8 entries counting tree itself.
         */
        std::filesystem::path makeTreeWithLinks()
        {
            const auto outside = root_.path() / "outside";
            std::filesystem::create_directories(outside);
            std::ofstream{outside / "keep.txt"} << "keep";
            const auto tree = root_.path() / "tree";
            std::filesystem::create_directories(tree / "sub" / "deeper");
            std::ofstream{tree / "a.txt"} << "a";
            std::ofstream{tree / "sub" / "b.txt"} << "bb";
            std::ofstream{tree / "sub" / "deeper" / "c.txt"} << "ccc";
            EXPECT_TRUE(makeDirectoryLink(tree / "sub" / "deeper" / "out", outside));
            std::error_code error;
            std::filesystem::create_symlink(outside / "keep.txt", tree / "fileLink", error);
            EXPECT_FALSE(error) << error.message();
            return tree;
        }

        TemporaryDirectory root_{temporaryRoot, true};
    };

    TEST(LinkReparseTagTests, OnlyNameSurrogatesAreLinks)
    {
        // IO_REPARSE_TAG_SYMLINK, IO_REPARSE_TAG_MOUNT_POINT (junction), IO_REPARSE_TAG_LX_SYMLINK
        EXPECT_TRUE(Detail::isLinkReparseTag(0xA000000Cu));
        EXPECT_TRUE(Detail::isLinkReparseTag(0xA0000003u));
        EXPECT_TRUE(Detail::isLinkReparseTag(0xA000001Du));
        // IO_REPARSE_TAG_CLOUD_6 (OneDrive placeholder), IO_REPARSE_TAG_DEDUP, IO_REPARSE_TAG_AF_UNIX,
        // IO_REPARSE_TAG_APPEXECLINK, IO_REPARSE_TAG_WOF
        EXPECT_FALSE(Detail::isLinkReparseTag(0x9000601Au));
        EXPECT_FALSE(Detail::isLinkReparseTag(0x80000013u));
        EXPECT_FALSE(Detail::isLinkReparseTag(0x80000023u));
        EXPECT_FALSE(Detail::isLinkReparseTag(0x8000001Bu));
        EXPECT_FALSE(Detail::isLinkReparseTag(0x80000017u));
        EXPECT_FALSE(Detail::isLinkReparseTag(0u));
    }

    TEST_F(LinkSafeRemoveTests, DirectoryLinkIsALinkAndADirectoryIsNot)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target);
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));

        std::error_code error;
        EXPECT_TRUE(isLinkOrJunction(root_.path() / "link", error));
        EXPECT_FALSE(error);
        EXPECT_FALSE(isLinkOrJunction(target, error));
        EXPECT_FALSE(isLinkOrJunction(root_.path() / "missing", error));
        EXPECT_FALSE(error) << "a missing path is no link and no error";
    }

    TEST_F(LinkSafeRemoveTests, RemovingATreeNeverDeletesThroughALink)
    {
        const auto outside = root_.path() / "outside";
        std::filesystem::create_directories(outside);
        std::ofstream{outside / "keep.txt"} << "keep";
        const auto tree = root_.path() / "tree";
        std::filesystem::create_directories(tree / "sub");
        std::ofstream{tree / "sub" / "own.txt"} << "own";
        ASSERT_TRUE(makeDirectoryLink(tree / "sub" / "link", outside));

        std::error_code error;
        const auto removed = removeAllNoFollow(tree, error);

        EXPECT_FALSE(error) << error.message();
        EXPECT_EQ(removed, 4u) << "tree, sub, own.txt and the link itself";
        EXPECT_FALSE(std::filesystem::exists(tree));
        EXPECT_TRUE(std::filesystem::exists(outside / "keep.txt")) << "the link target must survive";
    }

    TEST_F(LinkSafeRemoveTests, RemovingALinkKeepsItsTarget)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target);
        std::ofstream{target / "keep.txt"} << "keep";
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));

        std::error_code error;
        EXPECT_EQ(removeAllNoFollow(root_.path() / "link", error), 1u);
        EXPECT_FALSE(error) << error.message();
        EXPECT_TRUE(std::filesystem::exists(target / "keep.txt"));
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "link"));
    }

    TEST_F(LinkSafeRemoveTests, FirstLinkBelowFindsALinkedComponent)
    {
        const auto outside = root_.path() / "outside";
        std::filesystem::create_directories(outside);
        const auto tree = root_.path() / "tree";
        std::filesystem::create_directories(tree / "real");
        ASSERT_TRUE(makeDirectoryLink(tree / "real" / "link", outside));

        std::error_code error;
        const auto found = firstLinkBelow(tree, tree / "real" / "link" / "missing" / "file.txt", error);
        EXPECT_FALSE(error) << error.message();
        ASSERT_TRUE(found);
        EXPECT_EQ(*found, tree / "real" / "link");

        EXPECT_EQ(firstLinkBelow(tree, tree / "real" / "link", error), tree / "real" / "link") << "the leaf counts";
    }

    TEST_F(LinkSafeRemoveTests, FirstLinkBelowIgnoresTheRootAndMissingComponents)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target / "real");
        const auto linkedRoot = root_.path() / "linkedRoot";
        ASSERT_TRUE(makeDirectoryLink(linkedRoot, target));

        std::error_code error;
        EXPECT_FALSE(firstLinkBelow(linkedRoot, linkedRoot / "real" / "new" / "file.txt", error));
        EXPECT_FALSE(error) << error.message();
        EXPECT_FALSE(firstLinkBelow(linkedRoot, linkedRoot, error));
        EXPECT_FALSE(error) << error.message();
    }

    TEST_F(LinkSafeRemoveTests, FirstLinkBelowRefusesAPathOutsideTheRoot)
    {
        std::error_code error;
        EXPECT_FALSE(firstLinkBelow(root_.path() / "a", root_.path() / "b" / "file.txt", error));
        EXPECT_TRUE(error);
        EXPECT_FALSE(firstLinkBelow(root_.path() / "a", root_.path() / "a" / ".." / "b", error));
        EXPECT_TRUE(error);
    }

    TEST_F(LinkSafeRemoveTests, MissingPathRemovesNothing)
    {
        std::error_code error;
        EXPECT_EQ(removeAllNoFollow(root_.path() / "missing", error), 0u);
        EXPECT_FALSE(error);
    }

    TEST_F(LinkSafeRemoveTests, KindsAreInspectedWithoutFollowing)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target);
        std::ofstream{root_.path() / "file.txt"} << "file";
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));

        std::error_code error;
        EXPECT_EQ(noFollowKindOf(target, error), NoFollowKind::Directory);
        EXPECT_EQ(noFollowKindOf(root_.path() / "file.txt", error), NoFollowKind::Other);
        EXPECT_EQ(noFollowKindOf(root_.path() / "link", error), NoFollowKind::Link);
        EXPECT_EQ(noFollowKindOf(root_.path() / "missing", error), NoFollowKind::Missing);
        EXPECT_FALSE(error) << error.message();
    }

    TEST_F(LinkSafeRemoveTests, RemoveNoFollowRemovesOneItemOnly)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target / "full");
        std::ofstream{target / "full" / "keep.txt"} << "keep";
        std::filesystem::create_directories(target / "empty");
        std::ofstream{target / "file.txt"} << "file";
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));

        std::error_code error;
        EXPECT_TRUE(removeNoFollow(root_.path() / "link", error));
        EXPECT_FALSE(error) << error.message();
        EXPECT_TRUE(std::filesystem::exists(target / "full" / "keep.txt")) << "the link target must survive";

        EXPECT_TRUE(removeNoFollow(target / "file.txt", error));
        EXPECT_TRUE(removeNoFollow(target / "empty", error));
        EXPECT_FALSE(removeNoFollow(target / "full", error));
        EXPECT_TRUE(error) << "a directory with content is not removed";
        EXPECT_TRUE(std::filesystem::exists(target / "full" / "keep.txt"));

        EXPECT_FALSE(removeNoFollow(root_.path() / "missing", error));
        EXPECT_FALSE(error) << "a missing item is no error";
    }

#ifdef _WIN32
    TEST_F(LinkSafeRemoveTests, ReadOnlyItemsAreRemoved)
    {
        const auto tree = root_.path() / "readOnly";
        std::filesystem::create_directories(tree);
        std::ofstream{tree / "locked.txt"} << "locked";
        std::filesystem::permissions(tree / "locked.txt", std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);

        const auto result = removeAllNoFollow(tree);
        EXPECT_TRUE(result.failures.empty());
        EXPECT_EQ(result.removed, 2u);
        EXPECT_FALSE(std::filesystem::exists(tree));
    }
#endif

#ifndef _WIN32
    TEST_F(LinkSafeRemoveTests, AnUndeletableEntryKeepsOnlyItsDirectories)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root removes entries of read-only directories";

        const auto tree = root_.path() / "stuck";
        std::filesystem::create_directories(tree / "b-locked");
        std::filesystem::create_directories(tree / "c-free");
        std::ofstream{tree / "a.txt"} << "a";
        std::ofstream{tree / "b-locked" / "inner.txt"} << "inner";
        std::ofstream{tree / "c-free" / "free.txt"} << "free";
        std::ofstream{tree / "d.txt"} << "d";
        std::filesystem::permissions(tree / "b-locked", std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);

        const auto result = removeAllNoFollow(tree);
        std::filesystem::permissions(tree / "b-locked", std::filesystem::perms::owner_write, std::filesystem::perm_options::add);

        ASSERT_EQ(result.failures.size(), 1u);
        EXPECT_EQ(result.failures.front().path, tree / "b-locked" / "inner.txt") << "the real path that failed";
        EXPECT_TRUE(result.failures.front().error);
        EXPECT_EQ(result.removed, 4u) << "a.txt, c-free, free.txt and d.txt";
        EXPECT_TRUE(std::filesystem::exists(tree / "b-locked" / "inner.txt"));
        EXPECT_FALSE(std::filesystem::exists(tree / "a.txt"));
        EXPECT_FALSE(std::filesystem::exists(tree / "c-free"));
        EXPECT_FALSE(std::filesystem::exists(tree / "d.txt"));

        std::error_code error;
        removeAllNoFollow(tree, error);
        EXPECT_FALSE(error) << "the rest goes once it can";
    }

    TEST_F(LinkSafeRemoveTests, AnUnreadableDirectoryIsAFailureOfItsOwn)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root reads every directory";

        const auto tree = root_.path() / "unreadable";
        std::filesystem::create_directories(tree / "closed");
        std::ofstream{tree / "closed" / "inner.txt"} << "inner";
        std::ofstream{tree / "open.txt"} << "open";
        std::filesystem::permissions(tree / "closed", std::filesystem::perms::owner_read, std::filesystem::perm_options::remove);

        const auto result = removeAllNoFollow(tree);
        std::filesystem::permissions(tree / "closed", std::filesystem::perms::owner_read, std::filesystem::perm_options::add);

        ASSERT_EQ(result.failures.size(), 1u);
        EXPECT_EQ(result.failures.front().path, tree / "closed");
        EXPECT_FALSE(std::filesystem::exists(tree / "open.txt"));
        EXPECT_TRUE(std::filesystem::exists(tree / "closed" / "inner.txt"));
    }
#endif

    TEST_F(LinkSafeRemoveTests, CountingWalksTheTreeWithoutFollowingLinks)
    {
        const auto tree = makeTreeWithLinks();

        NoFollowTreeWalk walk{tree, NoFollowTreeWalk::Mode::Count};
        EXPECT_TRUE(walk.step(std::chrono::steady_clock::time_point::max()));
        EXPECT_TRUE(walk.done());
        EXPECT_EQ(walk.entries(), 8u) << "tree, a.txt, sub, b.txt, deeper, c.txt, out and fileLink";
        EXPECT_EQ(walk.bytes(), 6u) << "links count no bytes and are not followed";
        EXPECT_TRUE(walk.failures().empty());
        EXPECT_TRUE(std::filesystem::exists(tree / "sub" / "deeper" / "c.txt")) << "counting removes nothing";
    }

    TEST_F(LinkSafeRemoveTests, RemovingInSmallStepsDeletesTheTreeAndKeepsLinkTargets)
    {
        const auto tree = makeTreeWithLinks();

        NoFollowTreeWalk walk{tree, NoFollowTreeWalk::Mode::Remove};
        int steps = 0;
        std::set<std::filesystem::path> seen;
        while (!walk.step(std::chrono::steady_clock::time_point::min()))
        {
            ++steps;
            seen.insert(walk.lastPath());
            ASSERT_LT(steps, 100);
        }
        EXPECT_GE(steps, 7) << "an expired deadline still handles one entry per step";
        EXPECT_TRUE(seen.contains(tree / "sub" / "deeper" / "out"));
        EXPECT_EQ(walk.entries(), 8u);
        EXPECT_TRUE(walk.failures().empty());
        EXPECT_FALSE(std::filesystem::exists(tree));
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "outside" / "keep.txt"));
    }

    TEST_F(LinkSafeRemoveTests, WalkingALinkRootTouchesOnlyTheLink)
    {
        const auto target = root_.path() / "target";
        std::filesystem::create_directories(target);
        std::ofstream{target / "keep.txt"} << "keep";
        ASSERT_TRUE(makeDirectoryLink(root_.path() / "link", target));

        NoFollowTreeWalk count{root_.path() / "link", NoFollowTreeWalk::Mode::Count};
        count.step(std::chrono::steady_clock::time_point::max());
        EXPECT_EQ(count.entries(), 1u);

        NoFollowTreeWalk walk{root_.path() / "missing", NoFollowTreeWalk::Mode::Remove};
        EXPECT_TRUE(walk.step(std::chrono::steady_clock::time_point::max()));
        EXPECT_EQ(walk.entries(), 0u);
        EXPECT_TRUE(walk.failures().empty());
    }

    TEST_F(LinkSafeRemoveTests, VerifiedDirectoriesAreInspectedOnce)
    {
        const auto outside = root_.path() / "outside";
        std::filesystem::create_directories(outside);
        const auto tree = root_.path() / "tree";
        std::filesystem::create_directories(tree / "real" / "deeper");

        VerifiedDirectories verified;
        std::error_code error;
        EXPECT_FALSE(firstLinkBelow(tree, tree / "real" / "deeper" / "missing" / "file.txt", error, &verified));
        EXPECT_FALSE(error) << error.message();
        EXPECT_TRUE(verified.contains(tree / "real"));
        EXPECT_TRUE(verified.contains(tree / "real" / "deeper"));
        EXPECT_FALSE(verified.contains(tree / "real" / "deeper" / "missing")) << "a missing component is not verified";

        ASSERT_TRUE(makeDirectoryLink(tree / "real" / "deeper" / "missing", outside));
        EXPECT_EQ(
            firstLinkBelow(tree, tree / "real" / "deeper" / "missing" / "file.txt", error, &verified),
            tree / "real" / "deeper" / "missing"
        ) << "a component created later is inspected";
    }
}
