#pragma once

#include <ftp/protocol/reply_parsers.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace FileTransferProtocol::Test
{
    namespace
    {
        Reply replyOf(int code, std::string_view text)
        {
            return Reply{.code = code, .lines = {fmt::format("{} {}", code, text)}};
        }

        TimeVal timeOf(int year, unsigned month, unsigned day, int hour, int minute, int second)
        {
            return TimeVal{std::chrono::sys_days{std::chrono::year{year} / month / day}} + std::chrono::hours{hour} +
                std::chrono::minutes{minute} + std::chrono::seconds{second};
        }

        std::expected<std::string, RemoteIo::RemoteError> workingDirectoryOf(Reply const& reply)
        {
            return parsePathReply(reply, Verb::PrintWorkingDirectory);
        }

        template <typename ValueT>
        void expectViolation(std::expected<ValueT, RemoteIo::RemoteError> const& parsed)
        {
            ASSERT_FALSE(parsed);
            EXPECT_EQ(parsed.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
            EXPECT_EQ(parsed.error().protocol, RemoteIo::Protocol::Ftp);
        }
    }

    TEST(ReplyParserTests, SizeReply)
    {
        EXPECT_EQ(parseSizeReply(replyOf(213, "11")), 11u);
        EXPECT_EQ(parseSizeReply(replyOf(213, "0")), 0u);
        EXPECT_EQ(parseSizeReply(replyOf(213, " 42 ")), 42u);
        EXPECT_EQ(parseSizeReply(replyOf(213, "18446744073709551615")), std::numeric_limits<std::uint64_t>::max());
    }

    TEST(ReplyParserTests, SizeReplyThatIsNoSizeBreaksTheProtocol)
    {
        expectViolation(parseSizeReply(replyOf(213, "18446744073709551616")));
        expectViolation(parseSizeReply(replyOf(213, "-1")));
        expectViolation(parseSizeReply(replyOf(213, "12 bytes")));
        expectViolation(parseSizeReply(replyOf(213, "")));
        expectViolation(parseSizeReply(replyOf(213, "0x10")));
    }

    TEST(ReplyParserTests, SizeReplyOfAFailureIsItsError)
    {
        const auto parsed = parseSizeReply(replyOf(550, "listing/missing.txt: No such file or directory"));
        ASSERT_FALSE(parsed);
        EXPECT_EQ(parsed.error().kind, RemoteIo::RemoteErrorKind::NoSuchFile);
        EXPECT_EQ(parsed.error().protocolCode, 550);
    }

    TEST(ReplyParserTests, UnexpectedPositiveReplyBreaksTheProtocol)
    {
        expectViolation(parseSizeReply(replyOf(200, "11")));
        expectViolation(parsePassiveReply(replyOf(229, "(|||30003|)")));
        expectViolation(parseExtendedPassiveReply(replyOf(227, "(1,2,3,4,5,6)")));
        expectViolation(workingDirectoryOf(replyOf(250, "\"/home\"")));
    }

    TEST(ReplyParserTests, TimeVal)
    {
        EXPECT_EQ(parseTimeVal("20261001123456"), timeOf(2026, 10, 1, 12, 34, 56));
        EXPECT_EQ(parseTimeVal("10000101000000"), timeOf(1000, 1, 1, 0, 0, 0));
        EXPECT_EQ(parseTimeVal("99991231235959"), timeOf(9999, 12, 31, 23, 59, 59));
        EXPECT_EQ(parseTimeVal("20240229000000"), timeOf(2024, 2, 29, 0, 0, 0));
    }

    TEST(ReplyParserTests, TimeValWithFractions)
    {
        const auto base = timeOf(2026, 10, 1, 12, 34, 56);
        EXPECT_EQ(parseTimeVal("20261001123456.5"), base + std::chrono::milliseconds{500});
        EXPECT_EQ(parseTimeVal("20261001123456.123"), base + std::chrono::milliseconds{123});
        EXPECT_EQ(parseTimeVal("20261001123456.000001"), base + std::chrono::microseconds{1});
        EXPECT_EQ(parseTimeVal("20261001123456.1234569"), base + std::chrono::microseconds{123456});
    }

    TEST(ReplyParserTests, LeapSecondIsTheNextMinute)
    {
        EXPECT_EQ(parseTimeVal("20161231235960"), timeOf(2017, 1, 1, 0, 0, 0));
    }

    TEST(ReplyParserTests, TimeValOutOfRangeBreaksTheProtocol)
    {
        for (
            std::string_view text :
            {"09991231235959",
                "20261301000000",
                "20260001000000",
                "20260230000000",
                "20230229000000",
                "20261001240000",
                "20261001126000",
                "20261001123461",
                "2026100112345",
                "202610011234567",
                "20261001123456.",
                "20261001123456.5x",
                "2026-10-01 12:34",
                "191261001123456",
                ""}
        )
        {
            SCOPED_TRACE(text);
            expectViolation(parseTimeVal(text));
        }
    }

    TEST(ReplyParserTests, TimeValIsWrittenInWholeSeconds)
    {
        const auto time = std::chrono::sys_days{std::chrono::year{2020} / 3 / 4} + std::chrono::hours{5} +
            std::chrono::minutes{6} + std::chrono::seconds{7} + std::chrono::milliseconds{890};
        EXPECT_EQ(formatTimeVal(std::chrono::floor<std::chrono::seconds>(time)), "20200304050607");
        EXPECT_EQ(formatTimeVal(std::chrono::sys_seconds{}), "19700101000000");
        EXPECT_EQ(
            formatTimeVal(std::chrono::floor<std::chrono::seconds>(timeOf(9999, 12, 31, 23, 59, 59))), "99991231235959"
        );
        EXPECT_EQ(
            formatTimeVal(std::chrono::floor<std::chrono::seconds>(timeOf(1000, 1, 1, 0, 0, 0))), "10000101000000"
        );
    }

    TEST(ReplyParserTests, TimeValOutsideItsYearsIsNotWritten)
    {
        EXPECT_EQ(
            formatTimeVal(std::chrono::floor<std::chrono::seconds>(timeOf(999, 12, 31, 23, 59, 59))), std::nullopt
        );
        EXPECT_EQ(formatTimeVal(std::chrono::floor<std::chrono::seconds>(timeOf(10000, 1, 1, 0, 0, 0))), std::nullopt);
    }

    TEST(ReplyParserTests, TimeValBeyondTheNanosecondClockReadsBackAsItWas)
    {
        const auto time = parseTimeVal("40261001123456");
        ASSERT_TRUE(time);
        EXPECT_EQ(formatTimeVal(std::chrono::floor<std::chrono::seconds>(*time)), "40261001123456");
    }

    TEST(ReplyParserTests, ModificationTimeReply)
    {
        EXPECT_EQ(parseModificationTimeReply(replyOf(213, "20200102030405")), timeOf(2020, 1, 2, 3, 4, 5));
        EXPECT_EQ(
            parseModificationTimeReply(replyOf(213, "20200102030405.25")),
            timeOf(2020, 1, 2, 3, 4, 5) + std::chrono::milliseconds{250}
        );
        const auto broken = parseModificationTimeReply(replyOf(213, "2020"));
        expectViolation(broken);
        EXPECT_EQ(broken.error().protocolCode, 213);
        const auto failed = parseModificationTimeReply(replyOf(550, "Could not get file modification time."));
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().kind, RemoteIo::RemoteErrorKind::Failure);
    }

    TEST(ReplyParserTests, PassiveReplyWithParentheses)
    {
        const auto endpoint = parsePassiveReply(replyOf(227, "Entering Passive Mode (172,17,0,6,117,68)."));
        ASSERT_TRUE(endpoint);
        EXPECT_EQ(endpoint->address, (std::array<std::uint8_t, 4>{172, 17, 0, 6}));
        EXPECT_EQ(endpoint->port, 117 * 256 + 68);
    }

    TEST(ReplyParserTests, PassiveReplyWithoutParentheses)
    {
        for (
            std::string_view text :
            {"Entering Passive Mode 10,0,0,1,4,1",
                "=10,0,0,1,4,1",
                "10,0,0,1,4,1",
                "Entering Passive Mode (10,0,0,1,4,1"}
        )
        {
            SCOPED_TRACE(text);
            const auto endpoint = parsePassiveReply(replyOf(227, text));
            ASSERT_TRUE(endpoint);
            EXPECT_EQ(endpoint->address, (std::array<std::uint8_t, 4>{10, 0, 0, 1}));
            EXPECT_EQ(endpoint->port, 1025);
        }
    }

    TEST(ReplyParserTests, PassiveReplyScansPastOtherNumbers)
    {
        const auto endpoint = parsePassiveReply(replyOf(227, "Mode 2 on port 1,2 now (192,168,1,2,195,80)"));
        ASSERT_TRUE(endpoint);
        EXPECT_EQ(endpoint->address, (std::array<std::uint8_t, 4>{192, 168, 1, 2}));
        EXPECT_EQ(endpoint->port, 195 * 256 + 80);

        const auto pastLongNumber =
            parsePassiveReply(replyOf(227, "Session 12345678901234 Entering Passive Mode (10,0,0,1,4,5)"));
        ASSERT_TRUE(pastLongNumber);
        EXPECT_EQ(pastLongNumber->address, (std::array<std::uint8_t, 4>{10, 0, 0, 1}));
        EXPECT_EQ(pastLongNumber->port, 4 * 256 + 5);
    }

    TEST(ReplyParserTests, PassiveReplyOnAnyLineOfAMultiLineReply)
    {
        const Reply reply{.code = 227, .lines = {"227-Entering Passive Mode", "227 (10,0,0,1,4,1)"}};
        const auto endpoint = parsePassiveReply(reply);
        ASSERT_TRUE(endpoint);
        EXPECT_EQ(endpoint->port, 1025);
    }

    TEST(ReplyParserTests, PassiveReplyOnALineWithoutCode)
    {
        const Reply reply{.code = 227, .lines = {"227-Entering Passive Mode", "(10,0,0,1,4,2)", "227 OK"}};
        const auto endpoint = parsePassiveReply(reply);
        ASSERT_TRUE(endpoint);
        EXPECT_EQ(endpoint->port, 1026);
    }

    TEST(ReplyParserTests, PassiveReplyThatNamesNoEndpointBreaksTheProtocol)
    {
        for (
            std::string_view text :
            {"Entering Passive Mode (1,2,3,4,5)",
                "Entering Passive Mode (1,2,3,4,5,6,7)",
                "Entering Passive Mode (256,2,3,4,5,6)",
                "Entering Passive Mode (1,2,3,4,256,6)",
                "Entering Passive Mode (1,2,3,4,0,0)",
                "Entering Passive Mode (-1,2,3,4,5,6)",
                "Entering Passive Mode (1,2,3,4,99999999999,6)",
                "Entering Passive Mode",
                ""}
        )
        {
            SCOPED_TRACE(text);
            expectViolation(parsePassiveReply(replyOf(227, text)));
        }
    }

    TEST(ReplyParserTests, ExtendedPassiveReply)
    {
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "Entering Extended Passive Mode (|||30003|)")), 30003);
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "Extended Passive mode OK (|||30056|)")), 30056);
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "(!!!1!)")), 1);
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "(~~~65535~)")), 65535);
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "Mode (|||6446|")), 6446);
    }

    TEST(ReplyParserTests, ExtendedPassiveReplyScansPastOtherParentheses)
    {
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "Entering Extended Passive Mode (EPSV) (|||6446|)")), 6446);
        EXPECT_EQ(parseExtendedPassiveReply(replyOf(229, "Mode (|1|) ( (|||6447|)")), 6447);
    }

    TEST(ReplyParserTests, ExtendedPassiveReplyThatNamesNoPortBreaksTheProtocol)
    {
        for (
            std::string_view text :
            {"Entering Extended Passive Mode (|||30003!)",
                "Entering Extended Passive Mode (|!|30003|)",
                "Entering Extended Passive Mode (||||)",
                "Entering Extended Passive Mode (|||0|)",
                "Entering Extended Passive Mode (|||65536|)",
                "Entering Extended Passive Mode (|1|::1|30003|)",
                "Entering Extended Passive Mode (1112221)",
                "Entering Extended Passive Mode ( 30003 )",
                "Entering Extended Passive Mode (|||30003",
                "Entering Extended Passive Mode (",
                "Entering Extended Passive Mode |||30003|"}
        )
        {
            SCOPED_TRACE(text);
            expectViolation(parseExtendedPassiveReply(replyOf(229, text)));
        }
    }

    TEST(ReplyParserTests, PathReply)
    {
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"/home/spike\" is the current directory")), "/home/spike");
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"/\"")), "/");
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "Current directory is \"/srv\"")), "/srv");
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"/name with spaces\" created")), "/name with spaces");
    }

    TEST(ReplyParserTests, PathReplyUndoublesQuotes)
    {
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"/usr/dir\"\"name\" created")), "/usr/dir\"name");
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"\"\"\"")), "\"");
        EXPECT_EQ(workingDirectoryOf(replyOf(257, "\"a\"\"\"\"b\"")), "a\"\"b");
    }

    TEST(ReplyParserTests, PathReplyReadsCarriageReturnNulAsCarriageReturn)
    {
        EXPECT_EQ(workingDirectoryOf(replyOf(257, std::string{"\"/a\r\0b\" created", 15u})), "/a\rb");
    }

    TEST(ReplyParserTests, PathReplyErrorNamesTheCommandItAnswers)
    {
        const auto unexpected = parsePathReply(replyOf(250, "Directory created"), Verb::MakeDirectory);
        ASSERT_FALSE(unexpected);
        EXPECT_EQ(unexpected.error().kind, RemoteIo::RemoteErrorKind::ProtocolViolation);
        EXPECT_NE(unexpected.error().message.find("to MKD"), std::string::npos);

        const auto existing = parsePathReply(replyOf(550, "subdir: File exists"), Verb::MakeDirectory);
        ASSERT_FALSE(existing);
        EXPECT_EQ(existing.error().kind, RemoteIo::RemoteErrorKind::AlreadyExists);
    }

    TEST(ReplyParserTests, PathReplyWithoutAQuotedPathBreaksTheProtocol)
    {
        for (
            std::string_view text :
            {"/home/spike is the current directory", "\"/home/spike", "\"\" is cwd", "\"/a\"\"", ""}
        )
        {
            SCOPED_TRACE(text);
            expectViolation(workingDirectoryOf(replyOf(257, text)));
        }
    }
}
