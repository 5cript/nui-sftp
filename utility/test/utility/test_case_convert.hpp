#pragma once

#include <utility/algorithm/case_convert.hpp>
#include <utility/convert_naming_convention.hpp>

#include <gtest/gtest.h>

#include <string>

namespace Utility::Test
{
    TEST(CaseConvertTests, OnlyAsciiLettersChangeCase)
    {
        EXPECT_EQ(Algorithm::toLowerCase("ÄBC Größe"), "Äbc größe");
        EXPECT_EQ(Algorithm::toUpperCase("äbc größe"), "äBC GRößE");
        std::string text = "\xC3\x84X\xFF";
        Algorithm::toLowerCaseInplace(text);
        EXPECT_EQ(text, "\xC3\x84x\xFF");
    }

    TEST(CaseConvertTests, CompareIgnoresAsciiCaseOnly)
    {
        EXPECT_EQ(Algorithm::caseInsensitiveCompare("ABC", "abc"), 0);
        EXPECT_NE(Algorithm::caseInsensitiveCompare("Ä", "ä"), 0);
        EXPECT_LT(Algorithm::caseInsensitiveCompare("a", "\xC3\xA4"), 0);
    }

    TEST(CaseConvertTests, NamingConventionsKeepUtf8Intact)
    {
        const SegmentedString segments{.normalizedSegments = {"größe", "übung"}};
        EXPECT_EQ(segments.camelCase(), "größeübung");
        EXPECT_EQ(segments.screamingSnakeCase(), "größe_üBUNG");
    }
}
