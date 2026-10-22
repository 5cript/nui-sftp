#pragma once

#include <utility/checked_symlink_status.hpp>
#include <utility/temporary_directory.hpp>

#include <nui/utility/scope_exit.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <unistd.h>
#endif

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class CheckedSymlinkStatusTests : public ::testing::Test
    {
      protected:
        TemporaryDirectory directory_{temporaryRoot, true};
    };

    TEST_F(CheckedSymlinkStatusTests, AnExistingFileHasItsStatus)
    {
        std::ofstream{directory_.path() / "file.txt"} << "content";

        const auto status = checkedSymlinkStatus(directory_.path() / "file.txt");

        ASSERT_TRUE(status.has_value()) << status.error().message();
        ASSERT_TRUE(status->has_value());
        EXPECT_EQ((*status)->type(), std::filesystem::file_type::regular);
    }

    TEST_F(CheckedSymlinkStatusTests, AMissingPathIsNothing)
    {
        const auto status = checkedSymlinkStatus(directory_.path() / "missing.txt");

        ASSERT_TRUE(status.has_value()) << status.error().message();
        EXPECT_FALSE(status->has_value());
    }

    TEST_F(CheckedSymlinkStatusTests, APathBelowAFileIsNothing)
    {
        std::ofstream{directory_.path() / "file.txt"} << "content";

        const auto status = checkedSymlinkStatus(directory_.path() / "file.txt" / "inside.txt");

        ASSERT_TRUE(status.has_value()) << status.error().message();
        EXPECT_FALSE(status->has_value());
    }

    TEST_F(CheckedSymlinkStatusTests, ADanglingLinkIsALink)
    {
        std::error_code linkError;
        std::filesystem::create_symlink(directory_.path() / "nowhere", directory_.path() / "link", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();

        const auto status = checkedSymlinkStatus(directory_.path() / "link");

        ASSERT_TRUE(status.has_value()) << status.error().message();
        ASSERT_TRUE(status->has_value());
        EXPECT_EQ((*status)->type(), std::filesystem::file_type::symlink);
    }

#ifdef _WIN32
    /**
     * @brief A path past MAX_PATH is "not found" for std::filesystem without long path support, although it exists.
     */
    class CheckedSymlinkStatusLongPathTests : public CheckedSymlinkStatusTests
    {
      protected:
        void SetUp() override
        {
            const std::wstring segment(60, L's');
            longDirectory_ = directory_.path() / segment / segment / segment / segment / segment;
            ASSERT_GE(longDirectory_.native().size(), std::size_t{MAX_PATH});
            std::filesystem::create_directories(extended(longDirectory_));
        }

        void TearDown() override
        {
            std::error_code removeError;
            std::filesystem::remove_all(extended(directory_.path()), removeError);
        }

        static std::filesystem::path extended(std::filesystem::path const& path)
        {
            return std::filesystem::path{LR"(\\?\)" + path.lexically_normal().make_preferred().native()};
        }

        std::filesystem::path longDirectory_{};
    };

    TEST_F(CheckedSymlinkStatusLongPathTests, AnExistingPathPastMaxPathIsAnErrorNotGone)
    {
        std::ofstream{extended(longDirectory_ / "file.txt")} << "content";

        const auto directory = checkedSymlinkStatus(longDirectory_);
        const auto file = checkedSymlinkStatus(longDirectory_ / "file.txt");

        ASSERT_FALSE(directory.has_value());
        EXPECT_EQ(directory.error().value(), ERROR_FILENAME_EXCED_RANGE);
        ASSERT_FALSE(file.has_value());
        EXPECT_EQ(file.error().value(), ERROR_FILENAME_EXCED_RANGE);
    }

    TEST_F(CheckedSymlinkStatusLongPathTests, AMissingPathPastMaxPathIsNothing)
    {
        const auto status = checkedSymlinkStatus(longDirectory_ / "missing.txt");

        ASSERT_TRUE(status.has_value()) << status.error().message();
        EXPECT_FALSE(status->has_value());
    }

    TEST_F(CheckedSymlinkStatusLongPathTests, AnExtendedLengthPathHasItsStatus)
    {
        const auto status = checkedSymlinkStatus(extended(longDirectory_));

        ASSERT_TRUE(status.has_value()) << status.error().message();
        ASSERT_TRUE(status->has_value());
        EXPECT_EQ((*status)->type(), std::filesystem::file_type::directory);
    }
#else
    TEST_F(CheckedSymlinkStatusTests, APathThatCannotBeLookedAtIsAnErrorNotGone)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can look into any directory";
        const auto locked = directory_.path() / "locked";
        std::filesystem::create_directory(locked);
        std::ofstream{locked / "inside.txt"} << "content";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&locked]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        const auto inside = checkedSymlinkStatus(locked / "inside.txt");
        const auto missing = checkedSymlinkStatus(locked / "missing.txt");

        ASSERT_FALSE(inside.has_value());
        EXPECT_EQ(inside.error(), std::errc::permission_denied);
        EXPECT_FALSE(missing.has_value()) << "an unknown path must never count as gone";
    }

    TEST_F(CheckedSymlinkStatusTests, ANameTooLongToCheckIsAnErrorNotGone)
    {
        const auto status = checkedSymlinkStatus(directory_.path() / std::string(300, 'n'));

        ASSERT_FALSE(status.has_value());
        EXPECT_EQ(status.error(), std::errc::filename_too_long);
    }
#endif
}
