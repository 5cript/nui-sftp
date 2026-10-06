#pragma once

#include <utility/localized_message.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace Utility::Tests
{
    using namespace ::testing;

    class LocalizedMessageTests : public Test
    {};

    TEST_F(LocalizedMessageTests, KeyWithoutArgumentsStaysTheKey)
    {
        const auto message = localizedMessage("tests.example.key");
        EXPECT_EQ(message, "tests.example.key");

        const auto parsed = parseLocalizedMessage(message);
        EXPECT_EQ(parsed.key, "tests.example.key");
        EXPECT_TRUE(parsed.arguments.empty());
    }

    TEST_F(LocalizedMessageTests, ArgumentsRoundTrip)
    {
        const auto parsed = parseLocalizedMessage(localizedMessage("tests.example.key", "/some path/a.txt", 42, ""));
        EXPECT_EQ(parsed.key, "tests.example.key");
        EXPECT_EQ(parsed.arguments, (std::vector<std::string>{"/some path/a.txt", "42", ""}));
    }

    TEST_F(LocalizedMessageTests, PlainTextIsTheKey)
    {
        const auto parsed = parseLocalizedMessage("Connection refused: file.txt");
        EXPECT_EQ(parsed.key, "Connection refused: file.txt");
        EXPECT_TRUE(parsed.arguments.empty());
    }
}
