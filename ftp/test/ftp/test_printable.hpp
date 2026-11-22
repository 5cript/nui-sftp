#pragma once

#include <ftp/protocol/protocol_error.hpp>

#include <gtest/gtest.h>

#include <string>

namespace FileTransferProtocol::Test
{
    TEST(PrintableTests, ValidTextStays)
    {
        EXPECT_EQ(printable("Permission denied"), "Permission denied");
        EXPECT_EQ(printable("ünïcödé-名前 €"), "ünïcödé-名前 €");
    }

    TEST(PrintableTests, ControlCharactersOfAsciiAreEscaped)
    {
        EXPECT_EQ(printable(std::string{"a\0b\tc\x1B[31m\x7F", 11u}), "a\\x00b\\x09c\\x1B[31m\\x7F");
    }

    TEST(PrintableTests, ControlCharactersOfLatin1AreEscapedByteByByte)
    {
        EXPECT_EQ(
            printable(
                "a\xC2\x9B"
                "31mb\xC2\x85"
            ),
            "a\\xC2\\x9B31mb\\xC2\\x85"
        );
        EXPECT_EQ(printable("\xC2\xA0"), "\xC2\xA0");
    }

    TEST(PrintableTests, DirectionAndLineControlsAreEscaped)
    {
        EXPECT_EQ(
            printable(
                "invoice\xE2\x80\xAE"
                "fdp.exe"
            ),
            "invoice\\xE2\\x80\\xAEfdp.exe"
        );
        EXPECT_EQ(
            printable(
                "a\xE2\x81\xA6"
                "b\xE2\x81\xA9"
            ),
            "a\\xE2\\x81\\xA6b\\xE2\\x81\\xA9"
        );
        EXPECT_EQ(
            printable(
                "a\xE2\x80\x8F"
                "b\xD8\x9C"
            ),
            "a\\xE2\\x80\\x8Fb\\xD8\\x9C"
        );
        EXPECT_EQ(printable("line\xE2\x80\xA8next"), "line\\xE2\\x80\\xA8next");
        EXPECT_EQ(printable("zero\xE2\x80\x8Bwidth"), "zero\xE2\x80\x8Bwidth");
    }

    TEST(PrintableTests, InvalidUtf8IsEscapedByteByByte)
    {
        EXPECT_EQ(printable("caf\xFF\xC3("), "caf\\xFF\\xC3(");
        EXPECT_EQ(printable("\xE2\x82"), "\\xE2\\x82");
    }
}
