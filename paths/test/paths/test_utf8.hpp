#pragma once

#include <paths/utf8.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace Paths::Test
{
    TEST(Utf8Tests, SanitizedKeepsValidText)
    {
        for (const std::string_view text : {"", "plain", "Grüße 東京 \xF0\x9F\x9A\x80"})
            EXPECT_EQ(sanitizedUtf8(text), text);
    }

    TEST(Utf8Tests, SanitizedReplacesEveryInvalidByte)
    {
        EXPECT_EQ(sanitizedUtf8("a\xFF"
                                "b"),
                  "a\xEF\xBF\xBD"
                  "b");
        EXPECT_EQ(sanitizedUtf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");
        EXPECT_EQ(sanitizedUtf8("x\xC3"), "x\xEF\xBF\xBD");
        EXPECT_EQ(sanitizedUtf8("Fran\xE7ois"), "Fran\xEF\xBF\xBDois");
        EXPECT_TRUE(isValidUtf8(sanitizedUtf8("\xED\xA0\x80\xF4\x90\x80\x80\xF8\x88\x80\x80\x80")));
    }

    TEST(Utf8Tests, TruncatedNeverEndsInsideASequence)
    {
        EXPECT_EQ(truncatedUtf8("abc", 5u), "abc");
        EXPECT_EQ(truncatedUtf8("abc", 2u), "ab");
        EXPECT_EQ(truncatedUtf8("a\xC3\xA4", 2u), "a");
        EXPECT_EQ(truncatedUtf8("a\xC3\xA4", 3u), "a\xC3\xA4");
        EXPECT_EQ(truncatedUtf8("\xF0\x9F\x9A\x80x", 3u), "");
        EXPECT_EQ(truncatedUtf8("a\xFF\xFF", 2u), "a\xFF");
        EXPECT_EQ(truncatedUtf8("", 0u), "");
    }

    TEST(Utf8Tests, Utf16RoundTrip)
    {
        const std::string text = "Grüße 東京 \xF0\x9F\x9A\x80";
        const auto utf16 = utf8ToUtf16(text);
        EXPECT_EQ(utf16, u"Grüße 東京 \U0001F680");
        EXPECT_TRUE(isValidUtf16(std::u16string_view{utf16}));
        EXPECT_EQ(utf16ToUtf8(std::u16string_view{utf16}), text);
    }

    TEST(Utf8Tests, InvalidUtf8BecomesReplacementInUtf16)
    {
        EXPECT_EQ(utf8ToUtf16("a\xFF"), u"a�");
    }

    TEST(Utf8Tests, UnpairedSurrogatesBecomeReplacement)
    {
        const std::u16string lone{u'a', char16_t{0xD800u}, u'b', char16_t{0xDC00u}};
        EXPECT_FALSE(isValidUtf16(std::u16string_view{lone}));
        EXPECT_EQ(utf16ToUtf8(std::u16string_view{lone}), "a\xEF\xBF\xBD"
                                                          "b\xEF\xBF\xBD");
        const std::u16string trailingHigh{u'x', char16_t{0xDBFFu}};
        EXPECT_FALSE(isValidUtf16(std::u16string_view{trailingHigh}));
        EXPECT_EQ(utf16ToUtf8(std::u16string_view{trailingHigh}), "x\xEF\xBF\xBD");
    }
}
