#pragma once

#include <paths/paths.hpp>

#include <gtest/gtest.h>

namespace Paths::Test
{
    TEST(LocalPathTests, ParsesPosixPaths)
    {
        const auto path = LocalPath::fromUtf8("/home//tim/./sync/", LocalPlatform::Posix);
        ASSERT_TRUE(path.has_value());
        EXPECT_EQ(path->toUtf8(), "/home/tim/sync");
        EXPECT_EQ(LocalPath::fromUtf8("/home/tim/../root", LocalPlatform::Posix)->toUtf8(), "/home/root");
        EXPECT_EQ(LocalPath::fromUtf8("/..", LocalPlatform::Posix).error().reason, PathRejection::EscapesRoot);
        EXPECT_EQ(LocalPath::fromUtf8("relative", LocalPlatform::Posix).error().reason, PathRejection::NotAbsolute);
        // A backslash is a character on POSIX.
        EXPECT_EQ(LocalPath::fromUtf8("/a\\b", LocalPlatform::Posix)->filename()->value(), "a\\b");
    }

    TEST(LocalPathTests, ParsesWindowsPaths)
    {
        EXPECT_EQ(LocalPath::fromUtf8("C:\\Users\\tim\\", LocalPlatform::Windows)->toUtf8(), "C:/Users/tim");
        EXPECT_EQ(LocalPath::fromUtf8("C:/", LocalPlatform::Windows)->toUtf8(), "C:/");
        EXPECT_EQ(LocalPath::fromUtf8("c:", LocalPlatform::Windows)->toUtf8(), "c:/");
        EXPECT_EQ(LocalPath::fromUtf8("\\\\server\\share\\dir", LocalPlatform::Windows)->toUtf8(), "//server/share/dir");
        EXPECT_EQ(LocalPath::fromUtf8("/x", LocalPlatform::Windows).error().reason, PathRejection::NotAbsolute);
        EXPECT_EQ(LocalPath::fromUtf8("C:x", LocalPlatform::Windows).error().reason, PathRejection::NotAbsolute);
        EXPECT_EQ(
            LocalPath::fromUtf8("C:/dir/file.txt:stream", LocalPlatform::Windows).error().reason,
            PathRejection::ReservedCharacter
        );
    }

    TEST(LocalPathTests, ParentAndFilenameStopAtTheRoot)
    {
        const auto drive = LocalPath::fromUtf8("C:/a", LocalPlatform::Windows).value();
        EXPECT_EQ(drive.parent()->toUtf8(), "C:/");
        EXPECT_FALSE(drive.parent()->parent().has_value());
        EXPECT_EQ(LocalPath::fromUtf8("/a/b", LocalPlatform::Posix)->parent()->toUtf8(), "/a");
        EXPECT_EQ(LocalPath::fromUtf8("/a", LocalPlatform::Posix)->parent()->toUtf8(), "/");
    }

    TEST(LocalPathTests, JoiningRemoteNamesChecksTheLocalRules)
    {
        const auto relative = RelativePath::make("dir/a\\b").value();
        const auto posix = LocalPath::fromUtf8("/sync", LocalPlatform::Posix).value();
        EXPECT_EQ(posix.join(relative)->toUtf8(), "/sync/dir/a\\b");

        const auto windows = LocalPath::fromUtf8("C:/sync", LocalPlatform::Windows).value();
        EXPECT_EQ(windows.join(relative).error().reason, PathRejection::ReservedCharacter);
        EXPECT_EQ(windows.join(RelativePath::make("CON").value()).error().reason, PathRejection::ReservedDeviceName);
    }

    TEST(LocalPathTests, RelativeToStaysWithinTheBase)
    {
        const auto base = LocalPath::fromUtf8("/home/tim", LocalPlatform::Posix).value();
        EXPECT_EQ(LocalPath::fromUtf8("/home/tim/a/b", LocalPlatform::Posix)->relativeTo(base)->value(), "a/b");
        EXPECT_EQ(
            LocalPath::fromUtf8("/home/timothy", LocalPlatform::Posix)->relativeTo(base).error().reason,
            PathRejection::EscapesRoot
        );
        const auto root = LocalPath::fromUtf8("/", LocalPlatform::Posix).value();
        EXPECT_EQ(base.relativeTo(root)->value(), "home/tim");
    }

#ifndef __EMSCRIPTEN__
    TEST(LocalPathTests, ConvertsToAndFromNativePaths)
    {
        const auto native = std::filesystem::current_path() / "t\xC3\xA4st";
        const auto path = LocalPath::make(native);
        ASSERT_TRUE(path.has_value()) << path.error().toString();
        EXPECT_EQ(path->native(), native.lexically_normal());
        EXPECT_FALSE(LocalPath::make(std::filesystem::path{"relative"}).has_value());
    }
#endif
}
