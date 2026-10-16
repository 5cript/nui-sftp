#pragma once

#include <paths/native.hpp>
#include <paths/paths.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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
        EXPECT_EQ(LocalPath::fromUtf8("c:", LocalPlatform::Windows)->toUtf8(), "C:/");
        EXPECT_EQ(LocalPath::fromUtf8("d:\\Data", LocalPlatform::Windows)->toUtf8(), "D:/Data");
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

    TEST(LocalPathTests, Win32PrefixesAreDroppedAndDevicePathsRefused)
    {
        const auto windows = [](std::string_view text)
        {
            return LocalPath::fromUtf8(text, LocalPlatform::Windows);
        };
        EXPECT_EQ(windows("\\\\?\\c:\\Users\\tim")->toUtf8(), "C:/Users/tim");
        EXPECT_EQ(windows("\\\\?\\C:")->toUtf8(), "C:/");
        EXPECT_EQ(windows("\\\\?\\UNC\\server\\share\\dir")->toUtf8(), "//server/share/dir");
        EXPECT_EQ(windows("//?/unc/server/share")->toUtf8(), "//server/share/");
        EXPECT_EQ(*windows("\\\\?\\C:\\Users"), *windows("C:/Users"));
        for (auto const* text : {
                 "\\\\.\\PhysicalDrive0",
                 "\\\\.\\C:\\x",
                 "\\\\?\\Volume{12345678-1234-1234-1234-123456789abc}\\x",
                 "\\\\?\\GLOBALROOT\\Device\\x",
                 "//?/x",
                 "//../share/x",
                 "//./share",
             })
        {
            const auto path = windows(text);
            ASSERT_FALSE(path.has_value()) << text;
            EXPECT_EQ(path.error().reason, PathRejection::DevicePath) << text;
        }
        EXPECT_EQ(windows("\\\\?\\UNC\\server").error().reason, PathRejection::NotAbsolute);
        EXPECT_EQ(windows("\\\\?\\UNC\\..\\share").error().reason, PathRejection::DevicePath);

        // On POSIX these are ordinary names below the root.
        EXPECT_EQ(LocalPath::fromUtf8("//?/x", LocalPlatform::Posix)->toUtf8(), "/?/x");
    }

    TEST(LocalPathTests, RootLengthCoversDrivesAndShares)
    {
        using Detail::rootLengthOf;
        EXPECT_EQ(rootLengthOf("/a", LocalPlatform::Posix), 1u);
        EXPECT_EQ(rootLengthOf("a", LocalPlatform::Posix), 0u);
        EXPECT_EQ(rootLengthOf("C:/a", LocalPlatform::Windows), 3u);
        EXPECT_EQ(rootLengthOf("C:", LocalPlatform::Windows), 2u);
        EXPECT_EQ(rootLengthOf("C:a", LocalPlatform::Windows), 0u);
        EXPECT_EQ(rootLengthOf("1:/a", LocalPlatform::Windows), 0u);
        EXPECT_EQ(rootLengthOf("//server/share/a", LocalPlatform::Windows), 14u);
        EXPECT_EQ(rootLengthOf("//server/share", LocalPlatform::Windows), 14u);
        EXPECT_EQ(rootLengthOf("//server", LocalPlatform::Windows), 0u);
        EXPECT_EQ(rootLengthOf("//server/", LocalPlatform::Windows), 0u);
        EXPECT_EQ(rootLengthOf("///share", LocalPlatform::Windows), 0u);
        EXPECT_EQ(rootLengthOf("/a", LocalPlatform::Windows), 0u);
    }

    TEST(LocalPathTests, RefusesNulAndInvalidUtf8)
    {
        EXPECT_EQ(LocalPath::fromUtf8(std::string{"/a\0b", 4}, LocalPlatform::Posix).error().reason, PathRejection::ContainsNul);
        EXPECT_EQ(
            LocalPath::fromUtf8(std::string{"//ser\0ver/share", 16}, LocalPlatform::Windows).error().reason,
            PathRejection::ContainsNul
        );
        EXPECT_EQ(LocalPath::fromUtf8("/a/\xFF", LocalPlatform::Posix).error().reason, PathRejection::InvalidUtf8);
        EXPECT_EQ(LocalPath::fromUtf8("", LocalPlatform::Posix).error().reason, PathRejection::Empty);
    }

    TEST(LocalPathTests, WithinComparesThePlatformAndOptionallyIgnoresCase)
    {
        const auto windowsHome = LocalPath::fromUtf8("C:/Users/Me", LocalPlatform::Windows).value();
        const auto windowsUsers = LocalPath::fromUtf8("c:/USERS", LocalPlatform::Windows).value();
        EXPECT_FALSE(windowsHome.isWithin(windowsUsers));
        EXPECT_TRUE(windowsHome.isWithinIgnoringCase(windowsUsers));
        EXPECT_TRUE(windowsHome.isWithinIgnoringCase(windowsHome));
        EXPECT_TRUE(windowsHome.isWithinIgnoringCase(LocalPath::fromUtf8("C:/", LocalPlatform::Windows).value()));
        EXPECT_FALSE(windowsHome.isWithinIgnoringCase(LocalPath::fromUtf8("C:/Use", LocalPlatform::Windows).value()));
        EXPECT_FALSE(windowsHome.isWithinIgnoringCase(LocalPath::fromUtf8("D:/", LocalPlatform::Windows).value()));

        const auto posix = LocalPath::fromUtf8("/C:/Users/Me", LocalPlatform::Posix).value();
        EXPECT_FALSE(posix.isWithin(LocalPath::fromUtf8("/", LocalPlatform::Windows).value_or(LocalPath{})));
        EXPECT_FALSE(windowsHome.isWithin(LocalPath::fromUtf8("/", LocalPlatform::Posix).value()));
        EXPECT_FALSE(windowsHome.isWithinIgnoringCase(LocalPath::fromUtf8("/", LocalPlatform::Posix).value()));
        EXPECT_TRUE(posix.isWithin(LocalPath::fromUtf8("/", LocalPlatform::Posix).value()));
        EXPECT_FALSE(LocalPath{}.isWithin(LocalPath{}));

        const auto share = LocalPath::fromUtf8("//server/share/a", LocalPlatform::Windows).value();
        EXPECT_TRUE(share.isWithin(LocalPath::fromUtf8("//server/share", LocalPlatform::Windows).value()));
        EXPECT_FALSE(share.isWithin(LocalPath::fromUtf8("//server/sha", LocalPlatform::Windows).value()));
    }

    TEST(LocalPathTests, JoinAndAppendOnAnUnsetPathStayUnset)
    {
        const LocalPath unset{};
        EXPECT_EQ(unset.join(RelativePath::make("a").value()).error().reason, PathRejection::Empty);
        EXPECT_EQ(unset.join(RelativePath{}).error().reason, PathRejection::Empty);
        EXPECT_TRUE((unset / LocalPathSegment::make("a", LocalPlatform::Posix).value()).empty());
        EXPECT_FALSE(unset.parent().has_value());
        EXPECT_FALSE(unset.filename().has_value());

        const auto root = LocalPath::fromUtf8("/", LocalPlatform::Posix).value();
        EXPECT_EQ(root.join(RelativePath{})->toUtf8(), "/");
        EXPECT_EQ(root.join(RelativePath::make("a/b").value())->toUtf8(), "/a/b");
        const auto drive = LocalPath::fromUtf8("C:/", LocalPlatform::Windows).value();
        EXPECT_EQ(drive.join(RelativePath::make("a").value())->toUtf8(), "C:/a");
        EXPECT_EQ(drive.join(RelativePath::make("a/x.").value()).error().reason, PathRejection::TrailingDotOrSpace);
    }

    TEST(LocalPathTests, RoundTripsThroughJson)
    {
        const nlohmann::json json = LocalPath::fromUtf8("/home/t\xC3\xA4st").value();
        EXPECT_EQ(json.get<std::string>(), "/home/t\xC3\xA4st");
        EXPECT_EQ(json.get<LocalPath>().toUtf8(), "/home/t\xC3\xA4st");
        EXPECT_THROW(nlohmann::json{"relative"}.at(0).get<LocalPath>(), std::invalid_argument);
    }

#ifndef __EMSCRIPTEN__
    TEST(LocalPathTests, ConvertsToAndFromNativePaths)
    {
        const auto native = std::filesystem::current_path() / nativeFromUtf8("t\xC3\xA4st \xE6\x9D\xB1\xE4\xBA\xAC");
        const auto path = LocalPath::make(native);
        ASSERT_TRUE(path.has_value()) << path.error().toString();
        EXPECT_EQ(path->native(), native.lexically_normal());
        EXPECT_EQ(nativeToUtf8(path->native().filename()), "t\xC3\xA4st \xE6\x9D\xB1\xE4\xBA\xAC");
        EXPECT_FALSE(LocalPath::make(std::filesystem::path{"relative"}).has_value());
    }
#endif
}
