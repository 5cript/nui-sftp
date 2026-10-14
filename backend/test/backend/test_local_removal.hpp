#pragma once

#include <backend/local_removal.hpp>
#include <paths/native.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


extern std::filesystem::path temporaryRoot;

namespace Test
{
    class LocalRemovalTests : public ::testing::Test
    {
      protected:
        std::string utf8(std::filesystem::path const& path) const
        {
            return Paths::nativeToUtf8(path);
        }

        std::vector<Paths::LocalPath> homes() const
        {
            return {Paths::LocalPath::make(root_.path() / "home").value()};
        }

        Utility::TemporaryDirectory root_{temporaryRoot, true};
    };

    TEST_F(LocalRemovalTests, ARefusedPathRemovesNothing)
    {
        std::filesystem::create_directories(root_.path() / "home" / "user");
        std::ofstream{root_.path() / "file.txt"} << "file";
        const std::vector<std::string> paths{utf8(root_.path() / "file.txt"), utf8(root_.path() / "home")};

        const auto removal = removeLocalItems(paths, homes());
        ASSERT_FALSE(removal.has_value());
        EXPECT_EQ(removal.error().path, utf8(root_.path() / "home"));
        EXPECT_FALSE(removal.error().reason.empty());
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "file.txt"));

        const std::vector<std::string> relative{"relative/path"};
        EXPECT_FALSE(removeLocalItems(relative, homes()).has_value());
    }

    TEST_F(LocalRemovalTests, AFailureDoesNotStopTheOthers)
    {
        std::filesystem::create_directories(root_.path() / "full");
        std::ofstream{root_.path() / "full" / "keep.txt"} << "keep";
        std::ofstream{root_.path() / "a.txt"} << "a";
        std::ofstream{root_.path() / "b.txt"} << "b";
        const std::vector<std::string> paths{
            utf8(root_.path() / "a.txt"), utf8(root_.path() / "full"), utf8(root_.path() / "b.txt")
        };

        const auto removal = removeLocalItems(paths, homes());
        ASSERT_TRUE(removal.has_value());
        EXPECT_EQ(removal->removed, 2u);
        ASSERT_EQ(removal->failures.size(), 1u) << "a directory with content is not removed without recursion";
        const auto& failure = removal->failures.front();
        const auto path = utf8(root_.path() / "full");
        EXPECT_TRUE(failure.starts_with(path + ": "));
        EXPECT_EQ(failure.find(path, path.size()), std::string::npos) << "the path is named once";
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "a.txt"));
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "b.txt"));
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "full" / "keep.txt"));
    }

    TEST_F(LocalRemovalTests, ALinkToADirectoryIsRemovedItself)
    {
        std::filesystem::create_directories(root_.path() / "target");
        std::ofstream{root_.path() / "target" / "keep.txt"} << "keep";
        std::filesystem::create_directory_symlink(root_.path() / "target", root_.path() / "link");

        const std::vector<std::string> link{utf8(root_.path() / "link")};
        EXPECT_EQ(removeLocalItems(link, homes()).value().removed, 1u);
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(root_.path() / "link")));
        EXPECT_TRUE(std::filesystem::exists(root_.path() / "target" / "keep.txt"));
    }
}
