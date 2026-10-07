#pragma once

#include <utility/utf8.hpp>

#include <gtest/gtest.h>

#include <string>

namespace Utility::Tests
{
    using namespace ::testing;

    class Utf8Tests : public Test
    {};

    TEST_F(Utf8Tests, DecodesAsciiUnchanged)
    {
        EXPECT_EQ(Utf8::decode("Concurrency 42"), U"Concurrency 42");
    }

    TEST_F(Utf8Tests, DecodesMultibyteSequencesToCodePoints)
    {
        EXPECT_EQ(Utf8::decode("Größe"), U"Größe");
        EXPECT_EQ(Utf8::decode("并发"), U"并发");
        EXPECT_EQ(Utf8::decode("\xF0\x9F\x98\x80"), std::u32string{char32_t{0x1F600u}});
    }

    TEST_F(Utf8Tests, ReplacesMalformedInput)
    {
        const std::u32string replacement{char32_t{0xFFFDu}};
        EXPECT_EQ(Utf8::decode("\xFF"), replacement);
        EXPECT_EQ(Utf8::decode("a\xC3"), U"a" + replacement);
        EXPECT_EQ(
            Utf8::decode(
                "\xC3"
                "a"
            ),
            replacement + U"a"
        );
        EXPECT_EQ(Utf8::decode("\xC0\xAF"), replacement);
        EXPECT_EQ(Utf8::decode("\xED\xA0\x80"), replacement);
    }

    TEST_F(Utf8Tests, EncodeRoundTripsDecode)
    {
        for (const std::string text : {"plain", "Größe", "并发传输", "\xF0\x9F\x98\x80 mixed"})
            EXPECT_EQ(Utf8::encode(Utf8::decode(text)), text);
    }

    TEST_F(Utf8Tests, EncodeReplacesSurrogates)
    {
        EXPECT_EQ(Utf8::encode(std::u32string{char32_t{0xD800u}}), "\xEF\xBF\xBD");
    }
}
