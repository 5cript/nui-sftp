#pragma once

#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    class TemporaryDirectoryTests : public ::testing::Test
    {
      protected:
        /** @brief Longer than any file system takes for one name, so it cannot be made. */
        static std::string nameTooLong()
        {
            return std::string(300u, 'x');
        }

        TemporaryDirectory isolated_{temporaryRoot, true};
    };

    TEST_F(TemporaryDirectoryTests, MakesTheMissingDirectoriesAndRemovesItsOwnAtTheEnd)
    {
        const auto base = isolated_.path() / "a" / "b";
        std::filesystem::path made{};
        {
            const TemporaryDirectory directory{base, true};
            made = directory.path();
            EXPECT_TRUE(std::filesystem::is_directory(made));
            EXPECT_EQ(made.parent_path(), base);
        }
        EXPECT_FALSE(std::filesystem::exists(made));
        EXPECT_TRUE(std::filesystem::is_directory(base)) << "the directory it was made in is not its own";
    }

    TEST_F(TemporaryDirectoryTests, AFailedSetupRemovesTheDirectoriesItMadeOnTheWay)
    {
        // "a" and "b" can be made, the name behind them cannot.
        const auto firstMade = isolated_.path() / "a";

        EXPECT_THROW((TemporaryDirectory{firstMade / "b" / nameTooLong(), true}), std::runtime_error);

        EXPECT_FALSE(std::filesystem::exists(firstMade)) << "the constructor that threw left its directories behind";
    }

    TEST_F(TemporaryDirectoryTests, AFailedSetupLeavesWhatWasThereBefore)
    {
        const auto existing = isolated_.path() / "existing";
        ASSERT_TRUE(std::filesystem::create_directory(existing));
        {
            const std::ofstream kept{existing / "kept.txt"};
        }

        EXPECT_THROW((TemporaryDirectory{existing / "new" / nameTooLong(), true}), std::runtime_error);

        EXPECT_TRUE(std::filesystem::exists(existing / "kept.txt"));
        EXPECT_FALSE(std::filesystem::exists(existing / "new"));
    }

    TEST_F(TemporaryDirectoryTests, APathThatIsAFileIsRefusedAndStays)
    {
        const auto file = isolated_.path() / "file.txt";
        {
            const std::ofstream written{file};
        }

        EXPECT_THROW((TemporaryDirectory{file, true}), std::runtime_error);
        EXPECT_THROW((TemporaryDirectory{file / "below", true}), std::runtime_error);

        EXPECT_TRUE(std::filesystem::is_regular_file(file));
    }
}
