#pragma once

#include <utility/rename.hpp>
#include <utility/rename_target.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern std::filesystem::path programDirectory;
extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class RenameNoReplaceTests : public ::testing::Test
    {
      protected:
        static void write(std::filesystem::path const& path, std::string const& content)
        {
            std::ofstream{path, std::ios::binary} << content;
        }

        static std::string read(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(RenameNoReplaceTests, RenamesToAFreeName)
    {
        write(directory_.path() / "a", "a");
        ASSERT_TRUE(renameNoReplace(directory_.path() / "a", directory_.path() / "b").has_value());
        EXPECT_FALSE(std::filesystem::exists(directory_.path() / "a"));
        EXPECT_EQ(read(directory_.path() / "b"), "a");
    }

    TEST_F(RenameNoReplaceTests, RefusesToReplaceAFile)
    {
        write(directory_.path() / "a", "a");
        write(directory_.path() / "b", "b");
        const auto result = renameNoReplace(directory_.path() / "a", directory_.path() / "b");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::errc::file_exists);
        EXPECT_EQ(read(directory_.path() / "a"), "a");
        EXPECT_EQ(read(directory_.path() / "b"), "b");
    }

    TEST_F(RenameNoReplaceTests, RefusesToReplaceAnEmptyDirectory)
    {
        std::filesystem::create_directory(directory_.path() / "source");
        std::filesystem::create_directory(directory_.path() / "target");
        const auto result = renameNoReplace(directory_.path() / "source", directory_.path() / "target");
        ASSERT_FALSE(result.has_value());
        EXPECT_TRUE(std::filesystem::exists(directory_.path() / "source"));
    }

    TEST_F(RenameNoReplaceTests, RenamesDirectories)
    {
        std::filesystem::create_directory(directory_.path() / "source");
        write(directory_.path() / "source" / "inside", "x");
        ASSERT_TRUE(renameNoReplace(directory_.path() / "source", directory_.path() / "target").has_value());
        EXPECT_EQ(read(directory_.path() / "target" / "inside"), "x");
    }

    TEST_F(RenameNoReplaceTests, RenameUnlessExistsRenamesToAFreeName)
    {
        write(directory_.path() / "a", "a");
        ASSERT_TRUE(Detail::renameUnlessExists(directory_.path() / "a", directory_.path() / "b").has_value());
        EXPECT_FALSE(std::filesystem::exists(directory_.path() / "a"));
        EXPECT_EQ(read(directory_.path() / "b"), "a");
    }

    TEST_F(RenameNoReplaceTests, RenameUnlessExistsRefusesAnExistingFile)
    {
        write(directory_.path() / "a", "a");
        write(directory_.path() / "b", "b");
        const auto result = Detail::renameUnlessExists(directory_.path() / "a", directory_.path() / "b");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::errc::file_exists);
        EXPECT_EQ(read(directory_.path() / "a"), "a");
        EXPECT_EQ(read(directory_.path() / "b"), "b");
    }

#ifndef _WIN32
    TEST_F(RenameNoReplaceTests, RenameUnlessExistsRefusesADanglingLink)
    {
        write(directory_.path() / "a", "a");
        std::filesystem::create_symlink(directory_.path() / "missing", directory_.path() / "b");
        const auto result = Detail::renameUnlessExists(directory_.path() / "a", directory_.path() / "b");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::errc::file_exists);
        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(directory_.path() / "b")));
        EXPECT_EQ(read(directory_.path() / "a"), "a");
    }

    TEST_F(RenameNoReplaceTests, RenameByLinkMovesAFile)
    {
        write(directory_.path() / "a", "a");
        ASSERT_TRUE(Detail::renameByLink(directory_.path() / "a", directory_.path() / "b").has_value());
        EXPECT_FALSE(std::filesystem::exists(directory_.path() / "a"));
        EXPECT_EQ(read(directory_.path() / "b"), "a");
    }

    TEST_F(RenameNoReplaceTests, RenameByLinkRefusesAnExistingFile)
    {
        write(directory_.path() / "a", "a");
        write(directory_.path() / "b", "b");
        const auto result = Detail::renameByLink(directory_.path() / "a", directory_.path() / "b");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), std::errc::file_exists);
        EXPECT_FALSE(Detail::isNoReplaceUnsupported(result.error()));
        EXPECT_EQ(read(directory_.path() / "a"), "a");
        EXPECT_EQ(read(directory_.path() / "b"), "b");
    }
#endif

    TEST(RenameNoReplaceFallbackTests, OnlyMissingSupportFallsBack)
    {
        EXPECT_TRUE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::invalid_argument)));
        EXPECT_TRUE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::function_not_supported)));
        EXPECT_TRUE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::operation_not_supported)));
        EXPECT_TRUE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::not_supported)));
        EXPECT_TRUE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::operation_not_permitted)));
        EXPECT_FALSE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::file_exists)));
        EXPECT_FALSE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::no_such_file_or_directory)));
        EXPECT_FALSE(Detail::isNoReplaceUnsupported(std::make_error_code(std::errc::permission_denied)));
    }

    TEST_F(RenameNoReplaceTests, MoveIntoPlaceReplacesOnlyWhenAllowed)
    {
        write(directory_.path() / "a", "a");
        write(directory_.path() / "b", "b");
        const auto refused = moveIntoPlace(directory_.path() / "a", directory_.path() / "b", false);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error(), std::errc::file_exists);
        EXPECT_EQ(read(directory_.path() / "b"), "b");

        ASSERT_TRUE(moveIntoPlace(directory_.path() / "a", directory_.path() / "b", true).has_value());
        EXPECT_FALSE(std::filesystem::exists(directory_.path() / "a"));
        EXPECT_EQ(read(directory_.path() / "b"), "a");
    }

    TEST(RenameTargetTests, OnlyANewNameInTheSameDirectoryIsARenameInPlace)
    {
        const std::filesystem::path old{"/data/old.txt"};
        EXPECT_TRUE(isRenameInPlace(old, "/data/new.txt"));
        EXPECT_FALSE(isRenameInPlace(old, "/elsewhere/new.txt")) << "a move";
        EXPECT_FALSE(isRenameInPlace(old, "/data/sub/new.txt")) << "a move into a subdirectory";
        EXPECT_FALSE(isRenameInPlace(old, "/data/..")) << "the parent";
        EXPECT_FALSE(isRenameInPlace(old, "/data/.")) << "the directory itself";
        EXPECT_FALSE(isRenameInPlace(old, "/data/")) << "no name";
        EXPECT_FALSE(isRenameInPlace("old.txt", "new.txt")) << "relative paths have no parent to compare";
    }
}
