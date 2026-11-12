#pragma once

#include <ftp/protocol/reply.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        struct Framed
        {
            std::vector<Reply> replies{};
            std::optional<RemoteIo::RemoteError> error{};
            bool holdsPossibleEnd{false};
        };

        Framed
        frameInPieces(std::string_view bytes, std::vector<std::size_t> const& pieceSizes, ReplyLimits limits = {})
        {
            ReplyFramer framer{limits};
            Framed framed{};
            std::size_t position = 0u;
            for (std::size_t index = 0u; position < bytes.size(); ++index)
            {
                const auto size = pieceSizes.empty() ? bytes.size() : pieceSizes[index % pieceSizes.size()];
                const auto fed = framer.feed(bytes.substr(position, size));
                position += size;
                while (auto reply = framer.next())
                    framed.replies.push_back(std::move(*reply));
                if (!fed)
                {
                    framed.error = fed.error();
                    break;
                }
            }
            framed.holdsPossibleEnd = framer.holdsPossibleEnd();
            return framed;
        }

        Framed frameWhole(std::string_view bytes, ReplyLimits limits = {})
        {
            return frameInPieces(bytes, {}, limits);
        }

        void expectSameReplies(Framed const& expected, Framed const& actual)
        {
            ASSERT_EQ(expected.replies.size(), actual.replies.size());
            for (std::size_t index = 0u; index < expected.replies.size(); ++index)
            {
                EXPECT_EQ(expected.replies[index].code, actual.replies[index].code);
                EXPECT_EQ(expected.replies[index].lines, actual.replies[index].lines);
                EXPECT_EQ(expected.replies[index].droppedLines, actual.replies[index].droppedLines);
            }
            EXPECT_EQ(expected.error.has_value(), actual.error.has_value());
            EXPECT_EQ(expected.holdsPossibleEnd, actual.holdsPossibleEnd);
        }

        /**
         * @brief Frames @p bytes whole, byte by byte, split at every position and in seeded random pieces, and
         *        expects the same result each time (RFC 1123 4.1.2.10).
         */
        void expectFramingIgnoresReadBoundaries(std::string_view bytes)
        {
            const auto whole = frameWhole(bytes);
            expectSameReplies(whole, frameInPieces(bytes, {1u}));
            for (std::size_t split = 1u; split < bytes.size(); ++split)
            {
                SCOPED_TRACE(split);
                expectSameReplies(whole, frameInPieces(bytes, {split, bytes.size()}));
            }
            std::mt19937 random{4217u};
            for (int round = 0; round < 50; ++round)
            {
                std::vector<std::size_t> sizes{};
                for (int piece = 0; piece < 16; ++piece)
                    sizes.push_back(std::uniform_int_distribution<std::size_t>{1u, 7u}(random));
                SCOPED_TRACE(round);
                expectSameReplies(whole, frameInPieces(bytes, sizes));
            }
        }

        std::string withTelnet(std::initializer_list<int> bytes)
        {
            std::string text{};
            for (const auto byte : bytes)
                text += static_cast<char>(byte);
            return text;
        }
    }

    TEST(ReplyFramerTests, SingleLineReply)
    {
        const auto framed = frameWhole("220 Service ready\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 220);
        EXPECT_EQ(framed.replies[0].lines, std::vector<std::string>{"220 Service ready"});
        EXPECT_EQ(framed.replies[0].text(), "Service ready");
    }

    TEST(ReplyFramerTests, CodeWithoutTextIsAReply)
    {
        const auto framed = frameWhole("226\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 226);
        EXPECT_EQ(framed.replies[0].text(), "");
    }

    TEST(ReplyFramerTests, MultiLineReplyOfRfc959)
    {
        const auto framed =
            frameWhole("123-First line\r\nSecond line\r\n  234 A line beginning with numbers\r\n123 The last line\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 123);
        EXPECT_EQ(framed.replies[0].lines.size(), 4u);
        EXPECT_EQ(
            framed.replies[0].text(), "First line\nSecond line\n  234 A line beginning with numbers\nThe last line"
        );
        EXPECT_FALSE(framed.replies[0].endedOnOtherCode);
    }

    TEST(ReplyFramerTests, MultiLineReplyWithTheCodeOnEveryLine)
    {
        const auto framed = frameWhole("230-Welcome\r\n230-to the server\r\n230 Logged in\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 230);
        EXPECT_EQ(framed.replies[0].text(), "Welcome\nto the server\nLogged in");
    }

    TEST(ReplyFramerTests, LineWithTheOpeningCodeAndAHyphenDoesNotEndTheReply)
    {
        const auto framed = frameWhole("211-Features:\r\n211-More\r\n EPSV\r\n211 End\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines.size(), 4u);
    }

    TEST(ReplyFramerTests, PureFtpdGreeting)
    {
        const auto framed = frameWhole(
            "220---------- Welcome to Pure-FTPd [privsep] [TLS] ----------\r\n"
            "220-You are user number 1 of 50 allowed.\r\n"
            "220-Local time is now 23:48. Server port: 21.\r\n"
            "220 You will be disconnected after 60 seconds of inactivity.\r\n"
        );
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 220);
        EXPECT_EQ(framed.replies[0].lines.size(), 4u);
    }

    TEST(ReplyFramerTests, SeveralRepliesInOnePiece)
    {
        const auto framed =
            frameWhole("426 Failure writing network stream.\r\n225 No transfer to ABOR.\r\n257 \"/\"\r\n");
        ASSERT_EQ(framed.replies.size(), 3u);
        EXPECT_EQ(framed.replies[0].code, 426);
        EXPECT_EQ(framed.replies[1].code, 225);
        EXPECT_EQ(framed.replies[2].code, 257);
    }

    TEST(ReplyFramerTests, ReadBoundariesDoNotMatter)
    {
        expectFramingIgnoresReadBoundaries(
            "220-Welcome\r\n text\r\n220 Ready\r\n150 Opening\r\n226 Done\r\n211-Features:\r\n MDTM\r\n REST "
            "STREAM\r\n211 End\r\n"
        );
    }

    TEST(ReplyFramerTests, ReadBoundariesDoNotMatterForTelnetCommandsAndBareLineFeeds)
    {
        expectFramingIgnoresReadBoundaries(
            "220 Ready\n" + withTelnet({255, 251, 1}) + "331 Pass" + withTelnet({255, 255}) + "word\r\n230-a\n" +
            withTelnet({255, 254, 24, 255, 241}) + "x\r\n230 b\r\n"
        );
    }

    TEST(ReplyFramerTests, ReadBoundariesDoNotMatterForAPossibleEnd)
    {
        expectFramingIgnoresReadBoundaries(
            "230-Welcome\r\n200 users at most\r\nmore text\r\n230 Logged in\r\n226 Done\r\n"
        );
        expectFramingIgnoresReadBoundaries("230-Welcome\r\nmore\r\n200 Logged in\r\n");
    }

    TEST(ReplyFramerTests, BareLineFeedEndsALine)
    {
        const auto framed = frameWhole("220 Ready\n331 Password\n");
        ASSERT_EQ(framed.replies.size(), 2u);
        EXPECT_EQ(framed.replies[0].lines, std::vector<std::string>{"220 Ready"});
        EXPECT_EQ(framed.replies[1].code, 331);
    }

    TEST(ReplyFramerTests, BlankLinesBetweenRepliesAreSkipped)
    {
        const auto framed = frameWhole("\r\n220 Ready\r\n\r\n\n331 Password\r\n");
        ASSERT_EQ(framed.replies.size(), 2u);
        EXPECT_FALSE(framed.error);
    }

    TEST(ReplyFramerTests, BlankLineInsideAMultiLineReplyIsText)
    {
        const auto framed = frameWhole("220-Welcome\r\n\r\n220 Ready\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines.size(), 3u);
    }

    TEST(ReplyFramerTests, TelnetNegotiationIsRemoved)
    {
        const auto framed = frameWhole(
            withTelnet({255, 251, 1}) + "220 " + withTelnet({255, 253, 3}) + "Ready" +
            withTelnet({255, 252, 1, 255, 254, 3}) + "\r\n"
        );
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines, std::vector<std::string>{"220 Ready"});
    }

    TEST(ReplyFramerTests, InterpretAsCommandBeforeAnotherCommandIsAnUnescapedByte)
    {
        // CP1251 and Latin-1 names of a server that does not escape 0xFF, a subnegotiation start among them.
        const auto framed = frameWhole(
            "257 \"/home/\xFF\xFA"
            "dir\xFF\xF1\xF2\xFF\xF0\" created\r\n226 Done\r\n"
        );
        ASSERT_EQ(framed.replies.size(), 2u);
        EXPECT_EQ(
            framed.replies[0].lines.front(),
            "257 \"/home/\xFF\xFA"
            "dir\xFF\xF1\xF2\xFF\xF0\" created"
        );
        EXPECT_EQ(framed.replies[1].code, 226);
    }

    TEST(ReplyFramerTests, EscapedInterpretAsCommandIsOneByte)
    {
        const auto framed = frameWhole("550 caf" + withTelnet({255, 255}) + "\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines.front(), "550 caf\xFF");
        EXPECT_EQ(framed.replies[0].text(), "caf\\xFF");
    }

    TEST(ReplyFramerTests, UnescapedByteThatStartsNoTelnetCommandIsKept)
    {
        const auto framed = frameWhole(
            "550 \xFF"
            "a\r\n"
        );
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(
            framed.replies[0].lines.front(),
            "550 \xFF"
            "a"
        );
    }

    TEST(ReplyFramerTests, NulAndInvalidUtf8AreKeptAndShownEscaped)
    {
        const auto framed = frameWhole(std::string{"550 a\0b\xC3(\xE2\x82\xAC\r\n", 14u});
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines.front(), std::string("550 a\0b\xC3(\xE2\x82\xAC", 12u));
        EXPECT_EQ(framed.replies[0].text(), "a\\x00b\\xC3(\xE2\x82\xAC");
    }

    TEST(ReplyFramerTests, LinesThatStartNoReplyBreakTheProtocol)
    {
        for (
            std::string_view line :
            {"000 Zero", "999 Nine", "1234 Four digits", "abc Letters", "22 Short", "600 Six", "220x"}
        )
        {
            SCOPED_TRACE(line);
            const auto framed = frameWhole(std::string{line} + "\r\n");
            EXPECT_TRUE(framed.replies.empty());
            ASSERT_TRUE(framed.error);
            EXPECT_EQ(framed.error->kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
            EXPECT_EQ(framed.error->protocol, RemoteIo::Protocol::Ftp);
        }
    }

    TEST(ReplyFramerTests, NothingIsTakenAfterAViolation)
    {
        ReplyFramer framer{};
        ASSERT_FALSE(framer.feed("HTTP/1.1 400 Bad Request\r\n"));
        const auto again = framer.feed("220 Ready\r\n");
        ASSERT_FALSE(again);
        EXPECT_EQ(again.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
        EXPECT_FALSE(framer.next());
        const auto finished = framer.finish();
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
    }

    TEST(ReplyFramerTests, ViolationShowsTheStartOfALongLine)
    {
        const auto framed = frameWhole("<!DOCTYPE html><html><head><title>Not an FTP server</title></head>\r\n");
        ASSERT_TRUE(framed.error);
        EXPECT_EQ(framed.error->message, "Not a reply: '<!DOCTYPE html><html><head><title>Not an...'");
    }

    TEST(ReplyFramerTests, TooLongLineBreaksTheProtocolBeforeItsEnd)
    {
        const ReplyLimits limits{.lineLength = 16u};
        ReplyFramer framer{limits};
        EXPECT_TRUE(framer.feed("220 0123456789\r\n"));
        EXPECT_TRUE(framer.next());
        const auto fed = framer.feed(std::string(64u, 'x'));
        ASSERT_FALSE(fed);
        EXPECT_EQ(fed.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
    }

    TEST(ReplyFramerTests, UnescapedInterpretAsCommandCountsTowardsTheLineLength)
    {
        const ReplyLimits limits{.lineLength = 16u};
        ReplyFramer framer{limits};
        const auto fed = framer.feed("220 aaaaaaaaaaa\xFF\n");
        ASSERT_FALSE(fed);
        EXPECT_EQ(fed.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
        EXPECT_FALSE(framer.next());
    }

    TEST(ReplyFramerTests, EndlessLineIsRefusedAtTheDefaultLimit)
    {
        ReplyFramer framer{};
        const std::string chunk(1024u, 'a');
        bool refused = false;
        for (int round = 0; round < 64 && !refused; ++round)
            refused = !framer.feed(chunk);
        EXPECT_TRUE(refused);
    }

    TEST(ReplyFramerTests, LinesPastTheLineCountAreSkippedUpToTheEnd)
    {
        const ReplyLimits limits{.lineCount = 3u};
        const auto framed = frameWhole("211-a\r\n b\r\n c\r\n d\r\n e\r\n211 End\r\n220 Next\r\n", limits);
        ASSERT_EQ(framed.replies.size(), 2u);
        EXPECT_EQ(framed.replies[0].code, 211);
        EXPECT_EQ(framed.replies[0].lines, (std::vector<std::string>{"211-a", " b", " c", "211 End"}));
        EXPECT_EQ(framed.replies[0].droppedLines, 2u);
        EXPECT_EQ(framed.replies[1].code, 220);
    }

    TEST(ReplyFramerTests, LinesPastTheReplySizeAreSkippedUpToTheEnd)
    {
        const ReplyLimits limits{.replySize = 12u};
        const auto framed = frameWhole("211-abc\r\n defg\r\n hijklmn\r\n211 End\r\n", limits);
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].lines, (std::vector<std::string>{"211-abc", " defg", "211 End"}));
        EXPECT_EQ(framed.replies[0].droppedLines, 1u);
    }

    TEST(ReplyFramerTests, EndlessMultiLineReplyKeepsItsMemoryBounded)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("211-Start\r\n"));
        const std::string lines = []
        {
            std::string text{};
            for (int line = 0; line < 1000; ++line)
                text += " a line of an endless reply that never ends\r\n";
            return text;
        }();
        for (int round = 0; round < 20; ++round)
            ASSERT_TRUE(framer.feed(lines));
        EXPECT_TRUE(framer.inReply());
        ASSERT_TRUE(framer.feed("211 End\r\n"));
        const auto reply = framer.next();
        ASSERT_TRUE(reply);
        std::size_t kept = 0u;
        for (auto const& line : reply->lines)
            kept += line.size();
        EXPECT_LE(kept, ReplyLimits{}.replySize + std::string_view{"211 End"}.size());
        EXPECT_EQ(reply->lines.size() - 2u + reply->droppedLines, 20'000u);
    }

    TEST(ReplyFramerTests, LineWithAnotherCodeFollowedByTextIsText)
    {
        const auto framed = frameWhole("230-Welcome\r\n200 users at most\r\nmore text\r\n230 Logged in\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 230);
        EXPECT_EQ(framed.replies[0].lines.size(), 4u);
        EXPECT_FALSE(framed.holdsPossibleEnd);
    }

    TEST(ReplyFramerTests, LineWithAnotherCodeFollowedByTheEndIsText)
    {
        const auto framed = frameWhole("230-Welcome\r\n200 users at most\r\n230 Logged in\r\n");
        ASSERT_EQ(framed.replies.size(), 1u);
        EXPECT_EQ(framed.replies[0].code, 230);
        EXPECT_EQ(framed.replies[0].text(), "Welcome\n200 users at most\nLogged in");
    }

    TEST(ReplyFramerTests, LineWithAnotherCodeIsHeldAsAPossibleEnd)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("230-Welcome\r\nto glFTPd\r\n200 Logged in\r\n"));
        EXPECT_FALSE(framer.next());
        EXPECT_TRUE(framer.holdsPossibleEnd());
        EXPECT_TRUE(framer.inReply());

        framer.settle();
        const auto reply = framer.next();
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->code, 200);
        EXPECT_TRUE(reply->endedOnOtherCode);
        EXPECT_EQ(reply->text(), "Welcome\nto glFTPd\nLogged in");
        EXPECT_FALSE(framer.holdsPossibleEnd());
        EXPECT_FALSE(framer.inReply());
    }

    TEST(ReplyFramerTests, LaterLineWithAnotherCodeTakesOverThePossibleEnd)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("230-Welcome\r\n200 first\r\n250 second\r\n"));
        framer.settle();
        const auto reply = framer.next();
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->code, 250);
        EXPECT_EQ(reply->lines, (std::vector<std::string>{"230-Welcome", "200 first", "250 second"}));
    }

    TEST(ReplyFramerTests, SettleWaitsForAFollowingLineThatStarted)
    {
        for (std::string_view started : {"more te", "\xFF", "\xFF\xFB"})
        {
            SCOPED_TRACE(started);
            ReplyFramer framer{};
            ASSERT_TRUE(framer.feed(fmt::format("230-Welcome\r\n200 users at most\r\n{}", started)));
            framer.settle();
            EXPECT_FALSE(framer.next());

            ASSERT_TRUE(framer.feed("xt\r\n230 Logged in\r\n"));
            const auto reply = framer.next();
            ASSERT_TRUE(reply);
            EXPECT_EQ(reply->code, 230);
            EXPECT_FALSE(reply->endedOnOtherCode);
            EXPECT_EQ(reply->lines.size(), 4u);
        }
    }

    TEST(ReplyFramerTests, SettleWithoutAPossibleEndChangesNothing)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("230-Welcome\r\n"));
        framer.settle();
        EXPECT_FALSE(framer.next());
        EXPECT_TRUE(framer.inReply());
    }

    TEST(ReplyFramerTests, ALastLineWithoutLineEndIsALostConnection)
    {
        // Maybe the start of "213 1234".
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("213 12"));
        EXPECT_TRUE(framer.inReply());
        const auto finished = framer.finish();
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(finished.error().protocolCode, 213);
        EXPECT_FALSE(framer.next());
    }

    TEST(ReplyFramerTests, ALastLineWithoutLineEndCompletesNoOpenReply)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("211-Features:\r\n MDTM\r\n211 End"));
        const auto finished = framer.finish();
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(finished.error().protocolCode, 211);
        EXPECT_FALSE(framer.next());
    }

    TEST(ReplyFramerTests, FinishSettlesAPossibleEnd)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("421-Going down\r\n200 bye\r\n"));
        ASSERT_TRUE(framer.finish());
        const auto reply = framer.next();
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->code, 200);
    }

    TEST(ReplyFramerTests, FinishInTheMiddleOfAReplyIsALostConnection)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("211-Features:\r\n MDTM\r\n"));
        const auto finished = framer.finish();
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(finished.error().protocolCode, 211);
    }

    TEST(ReplyFramerTests, FinishOfALastLineThatStartsNoReplyBreaksTheProtocol)
    {
        ReplyFramer framer{};
        const auto finished = framer.feed("garbage").and_then(
            [&framer]()
            {
                return framer.finish();
            }
        );
        ASSERT_FALSE(finished);
        EXPECT_EQ(finished.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
    }

    TEST(ReplyFramerTests, NothingOpenAfterCompleteReplies)
    {
        ReplyFramer framer{};
        ASSERT_TRUE(framer.feed("220 Ready\r\n"));
        EXPECT_FALSE(framer.inReply());
        EXPECT_TRUE(framer.finish());
    }

    TEST(ReplyFramerTests, ReplyCodeOfALine)
    {
        EXPECT_EQ(replyCodeOf("220 Ready"), 220);
        EXPECT_EQ(replyCodeOf("220-Ready"), 220);
        EXPECT_EQ(replyCodeOf("220"), 220);
        EXPECT_EQ(replyCodeOf("220x"), std::nullopt);
        EXPECT_EQ(replyCodeOf(" 220 Ready"), std::nullopt);
        EXPECT_EQ(replyCodeOf("099 Low"), std::nullopt);
        EXPECT_EQ(replyCodeOf("22"), std::nullopt);
    }

    TEST(ReplyFramerTests, TextOfALine)
    {
        EXPECT_EQ(textOfLine("257 \"/home\" created"), "\"/home\" created");
        EXPECT_EQ(textOfLine("211-Features:"), "Features:");
        EXPECT_EQ(textOfLine("220"), "");
        EXPECT_EQ(textOfLine(" MDTM"), " MDTM");
        EXPECT_EQ(textOfLine("2000 bytes"), "2000 bytes");
    }
}
