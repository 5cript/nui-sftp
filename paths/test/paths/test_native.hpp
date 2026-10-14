#pragma once

#include <paths/local_path.hpp>
#include <paths/native.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace Paths::Test
{
    TEST(NativePathTests, NonAsciiTextSurvivesTheRoundTrip)
    {
        const std::string text = "ordner/Grüße 東京 🚀.txt";
        EXPECT_EQ(nativeToUtf8(nativeFromUtf8(text)), text);
    }

    TEST(NativePathTests, NameOfANativePathIsUtf8)
    {
        const auto native = nativeFromUtf8("Ärger.txt");
        EXPECT_EQ(nativeToUtf8(native.filename()), "Ärger.txt");
        EXPECT_EQ(native.filename().extension(), nativeFromUtf8(".txt"));
    }

    TEST(NativePathTests, ValidTextConvertsLosslessly)
    {
        const auto converted = convertNativeToUtf8(nativeFromUtf8("/home/Jürgen"));
        EXPECT_EQ(converted.utf8, "/home/Jürgen");
        EXPECT_TRUE(converted.lossless);
        EXPECT_EQ(checkedNativeToUtf8(nativeFromUtf8("/home/Jürgen")), "/home/Jürgen");
    }

#ifndef _WIN32
    TEST(NativePathTests, BytesThatAreNotUtf8AreReplacedAndReported)
    {
        const std::filesystem::path latin1{std::string{"/home/Fran\xE7ois"}};
        const auto converted = convertNativeToUtf8(latin1);
        EXPECT_EQ(converted.utf8, "/home/Fran\xEF\xBF\xBDois");
        EXPECT_FALSE(converted.lossless);
        EXPECT_EQ(nativeToUtf8(latin1), converted.utf8);

        const auto checked = checkedNativeToUtf8(latin1);
        ASSERT_FALSE(checked.has_value());
        EXPECT_EQ(checked.error().reason, PathRejection::InvalidUtf8);
    }

    TEST(NativePathTests, LocalPathRefusesANativePathThatIsNotUtf8)
    {
        const auto made = LocalPath::make(std::filesystem::path{std::string{"/home/Fran\xE7ois"}});
        ASSERT_FALSE(made.has_value());
        EXPECT_EQ(made.error().reason, PathRejection::InvalidUtf8);
    }
#else
    TEST(NativePathTests, UnpairedSurrogatesAreReplacedAndReported)
    {
        const std::wstring wide{L'C', L':', L'/', L'a', static_cast<wchar_t>(0xD800u)};
        const auto converted = convertNativeToUtf8(std::filesystem::path{wide});
        EXPECT_EQ(converted.utf8, "C:/a\xEF\xBF\xBD");
        EXPECT_FALSE(converted.lossless);
        const auto made = LocalPath::make(std::filesystem::path{wide});
        ASSERT_FALSE(made.has_value());
        EXPECT_EQ(made.error().reason, PathRejection::InvalidUtf8);
    }

    TEST(NativePathTests, TextThatIsNotUtf8BecomesReplacementCharacters)
    {
        EXPECT_EQ(nativeFromUtf8("a\xFF").native(), std::wstring{L"a\uFFFD"});
    }
#endif

#ifdef _WIN32
    TEST(NativePathTests, BackslashesBecomeForwardSlashes)
    {
        EXPECT_EQ(nativeToUtf8(std::filesystem::path{L"C:\\Users\\Jürgen"}), "C:/Users/Jürgen");
    }
#endif
}
