#pragma once

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

#ifdef _WIN32
    TEST(NativePathTests, BackslashesBecomeForwardSlashes)
    {
        EXPECT_EQ(nativeToUtf8(std::filesystem::path{L"C:\\Users\\Jürgen"}), "C:/Users/Jürgen");
    }
#endif
}
