#pragma once

#include <utility/extended_length_path.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace Utility::Test
{
#ifdef _WIN32
    TEST(ExtendedLengthPathTests, ADrivePathGetsThePrefixAndLosesItAgain)
    {
        const auto extended = extendedLengthPath(std::filesystem::path{L"C:/Users/me/./deep/../file.txt"});

        EXPECT_EQ(extended.native(), LR"(\\?\C:\Users\me\file.txt)");
        EXPECT_TRUE(isExtendedLengthPath(extended));
        EXPECT_EQ(withoutExtendedLengthPrefix(extended).native(), LR"(C:\Users\me\file.txt)");
    }

    TEST(ExtendedLengthPathTests, ANetworkPathGetsTheUncPrefixAndLosesItAgain)
    {
        const auto extended = extendedLengthPath(std::filesystem::path{LR"(\\server\share\file.txt)"});

        EXPECT_EQ(extended.native(), LR"(\\?\UNC\server\share\file.txt)");
        EXPECT_EQ(withoutExtendedLengthPrefix(extended).native(), LR"(\\server\share\file.txt)");
    }

    TEST(ExtendedLengthPathTests, RelativeAndPrefixedPathsStayAsTheyAre)
    {
        EXPECT_EQ(extendedLengthPath(std::filesystem::path{L"relative/file.txt"}).native(), L"relative/file.txt");
        EXPECT_EQ(extendedLengthPath(std::filesystem::path{LR"(\\?\C:\a\..\b)"}).native(), LR"(\\?\C:\a\..\b)");
        EXPECT_EQ(extendedLengthPath(std::filesystem::path{LR"(\\.\pipe\name)"}).native(), LR"(\\.\pipe\name)");
        EXPECT_FALSE(isExtendedLengthPath(std::filesystem::path{L"C:/file.txt"}));
        EXPECT_EQ(withoutExtendedLengthPrefix(std::filesystem::path{L"C:/file.txt"}).native(), L"C:/file.txt");
    }
#else
    TEST(ExtendedLengthPathTests, PathsStayAsTheyAreOutsideWindows)
    {
        const std::filesystem::path path{"/home/me/./file.txt"};

        EXPECT_EQ(extendedLengthPath(path), path);
        EXPECT_FALSE(isExtendedLengthPath(path));
        EXPECT_EQ(withoutExtendedLengthPrefix(path), path);
    }
#endif
}
