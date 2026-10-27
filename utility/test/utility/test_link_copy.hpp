#pragma once

#include <utility/link_copy.hpp>
#include <utility/link_safe_remove.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class LinkCopyTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            std::filesystem::create_directories(target_);
            std::ofstream{target_ / "keep.txt"} << "keep";
        }

        static std::string read(std::filesystem::path const& path)
        {
            std::ifstream stream{path, std::ios::binary};
            return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }

        TemporaryDirectory root_{temporaryRoot, true};
        std::filesystem::path target_{root_.path() / "target"};
    };

    TEST_F(LinkCopyTests, ASymlinkIsCopiedWithItsTarget)
    {
        std::error_code error{};
        std::filesystem::create_symlink("target/keep.txt", root_.path() / "link", error);
        if (error)
            GTEST_SKIP() << "symlinks cannot be created here: " << error.message();

        const auto copied = copyLink(root_.path() / "link", root_.path() / "copy");

        ASSERT_TRUE(copied.has_value()) << copied.error().message();
        EXPECT_TRUE(std::filesystem::is_symlink(root_.path() / "copy"));
        EXPECT_EQ(std::filesystem::read_symlink(root_.path() / "copy"), std::filesystem::path{"target/keep.txt"});
        EXPECT_EQ(read(root_.path() / "copy"), "keep");
    }

    TEST_F(LinkCopyTests, AnExistingDestinationIsNotReplaced)
    {
        std::error_code error{};
        std::filesystem::create_symlink("target/keep.txt", root_.path() / "link", error);
        if (error)
            GTEST_SKIP() << "symlinks cannot be created here: " << error.message();
        std::ofstream{root_.path() / "copy"} << "mine";

        EXPECT_FALSE(copyLink(root_.path() / "link", root_.path() / "copy").has_value());
        EXPECT_EQ(read(root_.path() / "copy"), "mine");
    }

#ifdef _WIN32
    TEST_F(LinkCopyTests, AJunctionIsCopiedAsAJunctionToTheSameTarget)
    {
        const auto junction = root_.path() / "junction";
        const auto command = "cmd /c mklink /J \"" + junction.string() + "\" \"" + target_.string() + "\" >nul";
        ASSERT_EQ(std::system(command.c_str()), 0);

        const auto copied = copyLink(junction, root_.path() / "copy");

        ASSERT_TRUE(copied.has_value()) << copied.error().message();
        std::error_code error{};
        EXPECT_EQ(noFollowKindOf(root_.path() / "copy", error), NoFollowKind::Link);
        EXPECT_FALSE(std::filesystem::is_symlink(root_.path() / "copy")) << "a junction, not a symlink";
        EXPECT_TRUE(std::filesystem::equivalent(root_.path() / "copy", target_));
        EXPECT_EQ(read(root_.path() / "copy" / "keep.txt"), "keep");

        EXPECT_TRUE(removeNoFollow(root_.path() / "copy", error)) << error.message();
        EXPECT_EQ(read(target_ / "keep.txt"), "keep") << "removing the copy keeps the target";
    }

    TEST_F(LinkCopyTests, AJunctionIsNotCopiedOntoAnExistingDirectory)
    {
        const auto junction = root_.path() / "junction";
        const auto command = "cmd /c mklink /J \"" + junction.string() + "\" \"" + target_.string() + "\" >nul";
        ASSERT_EQ(std::system(command.c_str()), 0);
        std::filesystem::create_directories(root_.path() / "copy");
        std::ofstream{root_.path() / "copy" / "mine.txt"} << "mine";

        EXPECT_FALSE(copyLink(junction, root_.path() / "copy").has_value());
        EXPECT_EQ(read(root_.path() / "copy" / "mine.txt"), "mine");
    }
#endif
}
