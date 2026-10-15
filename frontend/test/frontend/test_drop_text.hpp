#pragma once

#include <nui-file-explorer/drop_text.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Test
{
    using NuiFileExplorer::Detail::droppedLines;
    using NuiFileExplorer::Detail::droppedPathOf;
    using NuiFileExplorer::Detail::extractFromHtmlLink;
    using NuiFileExplorer::Detail::pathOfFileUrl;
    using NuiFileExplorer::Detail::percentDecoded;
    using Paths::LocalPlatform;

    TEST(DropTextTests, PercentEscapesAreDecoded)
    {
        EXPECT_EQ(percentDecoded("/home/my%20file"), "/home/my file");
        EXPECT_EQ(percentDecoded("/%C3%A4"), "/\xC3\xA4");
        EXPECT_EQ(percentDecoded("/50%"), "/50%") << "an incomplete escape stays";
        EXPECT_EQ(percentDecoded("/%zz"), "/%zz");
    }

    TEST(DropTextTests, DecodedNulOrInvalidUtf8IsRejected)
    {
        EXPECT_FALSE(percentDecoded("/home/a%00b"));
        EXPECT_FALSE(percentDecoded("/home/%FF"));
        EXPECT_FALSE(percentDecoded("/home/%C3"));
    }

    TEST(DropTextTests, LocalFileUrlsNameLocalPaths)
    {
        EXPECT_EQ(pathOfFileUrl("file:///home/x", LocalPlatform::Posix), "/home/x");
        EXPECT_EQ(pathOfFileUrl("file://localhost/home/x", LocalPlatform::Posix), "/home/x");
        EXPECT_EQ(pathOfFileUrl("file:/home/x", LocalPlatform::Posix), "/home/x");
        EXPECT_FALSE(pathOfFileUrl("file://", LocalPlatform::Posix));
        EXPECT_FALSE(pathOfFileUrl("file:x", LocalPlatform::Posix));
    }

    TEST(DropTextTests, OtherHostsAreUncOnWindowsOnly)
    {
        EXPECT_FALSE(pathOfFileUrl("file://server/share/x", LocalPlatform::Posix));
        EXPECT_EQ(pathOfFileUrl("file://server/share/x", LocalPlatform::Windows), "//server/share/x");
    }

    TEST(DropTextTests, DriveLetterIsUnwrappedOnWindowsOnly)
    {
        EXPECT_EQ(pathOfFileUrl("file:///C:/Users/x", LocalPlatform::Windows), "C:/Users/x");
        EXPECT_EQ(pathOfFileUrl("file:///C:", LocalPlatform::Windows), "C:");
        EXPECT_EQ(pathOfFileUrl("file:///C:/Users/x", LocalPlatform::Posix), "/C:/Users/x");
        EXPECT_EQ(pathOfFileUrl("file:///ab:c", LocalPlatform::Windows), "/ab:c") << "only /X: is a drive";
        EXPECT_EQ(pathOfFileUrl("file:///C:x", LocalPlatform::Windows), "/C:x");
    }

    TEST(DropTextTests, DroppedTextMustBeAnAbsolutePathOfThePlatform)
    {
        EXPECT_EQ(droppedPathOf("/home/my file", LocalPlatform::Posix), "/home/my file");
        EXPECT_EQ(droppedPathOf("file:///home/my%20file", LocalPlatform::Posix), "/home/my file");
        EXPECT_FALSE(droppedPathOf("C:\\Users\\x", LocalPlatform::Posix));
        EXPECT_FALSE(droppedPathOf("and/or", LocalPlatform::Posix));
        EXPECT_FALSE(droppedPathOf("", LocalPlatform::Posix));
        EXPECT_FALSE(droppedPathOf(std::string_view{"/a\0b", 4}, LocalPlatform::Posix));
        EXPECT_FALSE(droppedPathOf("/home/\xFF", LocalPlatform::Posix));
        EXPECT_FALSE(droppedPathOf("file:///home/a%00b", LocalPlatform::Posix));

        EXPECT_EQ(droppedPathOf("C:\\Users\\x", LocalPlatform::Windows), "C:\\Users\\x");
        EXPECT_EQ(droppedPathOf("C:/Users/x", LocalPlatform::Windows), "C:/Users/x");
        EXPECT_EQ(droppedPathOf("\\\\server\\share\\x", LocalPlatform::Windows), "\\\\server\\share\\x");
        EXPECT_EQ(droppedPathOf("file:///C:/Users/x", LocalPlatform::Windows), "C:/Users/x");
        EXPECT_FALSE(droppedPathOf("/home/x", LocalPlatform::Windows)) << "no drive";
        EXPECT_FALSE(droppedPathOf("file:///home/x", LocalPlatform::Windows));
        EXPECT_FALSE(droppedPathOf("file://server/", LocalPlatform::Windows)) << "no share";
    }

    TEST(DropTextTests, TextSplitsIntoTrimmedLines)
    {
        EXPECT_EQ(
            droppedLines("file:///home/a b\r\n  /home/c d  \n\n\t\n/e"),
            (std::vector<std::string_view>{"file:///home/a b", "/home/c d", "/e"})
        );
        EXPECT_TRUE(droppedLines("").empty());
        EXPECT_TRUE(droppedLines(" \n\r\n").empty());
    }

    TEST(DropTextTests, FileUrlOfTheLastLinkIsExtracted)
    {
        EXPECT_EQ(extractFromHtmlLink("<a href=\"file:///home/x\">file:///home/x</a>"), "file:///home/x");
        EXPECT_FALSE(extractFromHtmlLink("<a href=\"https://x\">https://x</a>"));
        EXPECT_FALSE(extractFromHtmlLink("<b>file:///home/x</b>"));
        EXPECT_FALSE(extractFromHtmlLink("file:///home/x</a>"));
    }
}
