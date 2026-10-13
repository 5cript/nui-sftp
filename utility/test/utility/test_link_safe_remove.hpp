#pragma once

#include <utility/link_safe_remove.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

        TemporaryDirectory root_{temporaryRoot, true};
    };

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
}
