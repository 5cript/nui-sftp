#pragma once

#include <ftp/protocol/reply_code.hpp>

#include <gtest/gtest.h>

namespace FileTransferProtocol::Test
{
    TEST(ReplyCodeTests, FirstDigitGivesTheClass)
    {
        EXPECT_EQ(classifyReply(150), ReplyClass::PositivePreliminary);
        EXPECT_EQ(classifyReply(226), ReplyClass::PositiveCompletion);
        EXPECT_EQ(classifyReply(331), ReplyClass::PositiveIntermediate);
        EXPECT_EQ(classifyReply(421), ReplyClass::TransientNegative);
        EXPECT_EQ(classifyReply(550), ReplyClass::PermanentNegative);
    }

    TEST(ReplyCodeTests, UnknownCodesUseTheHighDigit)
    {
        EXPECT_EQ(classifyReply(299), ReplyClass::PositiveCompletion);
        EXPECT_EQ(classifyReply(599), ReplyClass::PermanentNegative);
    }

    TEST(ReplyCodeTests, CodesOutsideTheClassesHaveNone)
    {
        EXPECT_EQ(classifyReply(99), std::nullopt);
        EXPECT_EQ(classifyReply(600), std::nullopt);
        EXPECT_EQ(classifyReply(-226), std::nullopt);
    }
}
