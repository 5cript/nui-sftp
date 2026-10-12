#pragma once

#include <paths/paths.hpp>

#include <gtest/gtest.h>

namespace Paths::Test
{
    TEST(RemotePathTests, NormalizesSeparatorsAndDots)
    {
        EXPECT_EQ(RemotePath::make("/home//test/./a/")->value(), "/home/test/a");
        EXPECT_EQ(RemotePath::make("/")->value(), "/");
        EXPECT_EQ(RemotePath::make("//")->value(), "/");
    }

    TEST(RemotePathTests, RefusesRelativeAndParentReferences)
    {
        EXPECT_EQ(RemotePath::make("home").error().reason, PathRejection::NotAbsolute);
        EXPECT_EQ(RemotePath::make("").error().reason, PathRejection::Empty);
        EXPECT_EQ(RemotePath::make("/home/..").error().reason, PathRejection::DotOrDotDot);
        EXPECT_EQ(RemotePath::make("/a/../b").error().reason, PathRejection::DotOrDotDot);
    }

    TEST(RemotePathTests, BackslashesAndColonsAreOrdinaryCharacters)
    {
        const auto base = RemotePath::make("/home/test").value();
        const auto joined = base / PathSegment::make("a\\..\\b").value();
        EXPECT_EQ(joined.value(), "/home/test/a\\..\\b");
        EXPECT_EQ(joined.parent()->value(), "/home/test");
        EXPECT_EQ(joined.filename()->value(), "a\\..\\b");

        // A drive-like name never replaces the base like std::filesystem::path("/x") / "C:y" does on Windows.
        EXPECT_EQ((base / PathSegment::make("C:notes").value()).value(), "/home/test/C:notes");
    }

    TEST(RemotePathTests, ParentAndFilenameStopAtTheRoot)
    {
        EXPECT_FALSE(RemotePath::root().parent().has_value());
        EXPECT_FALSE(RemotePath::root().filename().has_value());
        EXPECT_EQ(RemotePath::make("/a")->parent()->value(), "/");
    }

    TEST(RemotePathTests, RelativeToStaysWithinTheBase)
    {
        const auto base = RemotePath::make("/home/test").value();
        EXPECT_EQ(RemotePath::make("/home/test/a/b")->relativeTo(base)->value(), "a/b");
        EXPECT_EQ(RemotePath::make("/home/test")->relativeTo(base)->value(), "");
        EXPECT_EQ(RemotePath::make("/home/testing")->relativeTo(base).error().reason, PathRejection::EscapesRoot);
        EXPECT_EQ(RemotePath::make("/etc")->relativeTo(base).error().reason, PathRejection::EscapesRoot);
        EXPECT_EQ(RemotePath::make("/a/b")->relativeTo(RemotePath::root())->value(), "a/b");
    }

    TEST(RemotePathTests, JoiningARelativePathCannotEscape)
    {
        const auto base = RemotePath::make("/srv/sync").value();
        const auto joined = base / RelativePath::make("x/y").value();
        EXPECT_EQ(joined.value(), "/srv/sync/x/y");
        EXPECT_TRUE(joined.isWithin(base));
    }

    TEST(RemotePathTests, RoundTripsThroughJson)
    {
        const nlohmann::json json = RemotePath::make("/home/t\xC3\xA4st/a\\b").value();
        EXPECT_EQ(json.get<std::string>(), "/home/t\xC3\xA4st/a\\b");
        EXPECT_EQ(json.get<RemotePath>().value(), "/home/t\xC3\xA4st/a\\b");
        EXPECT_THROW(nlohmann::json{"relative"}.at(0).get<RemotePath>(), std::invalid_argument);
    }

    TEST(RemotePathTests, FilenameSuffixStaysInTheSameDirectory)
    {
        EXPECT_EQ(RemotePath::make("/home/a.txt")->withFilenameSuffix(".filepart")->value(), "/home/a.txt.filepart");
        EXPECT_EQ(RemotePath::make("/a")->withFilenameSuffix("~")->value(), "/a~");
        EXPECT_EQ(RemotePath::make("/home/a")->withFilenameSuffix("/../x").error().reason, PathRejection::ContainsSeparator);
        EXPECT_FALSE(RemotePath::root().withFilenameSuffix(".old").has_value());
        EXPECT_FALSE(RemotePath{}.withFilenameSuffix(".old").has_value());
    }

    TEST(RemotePathTests, LinksResolveFromTheirDirectory)
    {
        const auto link = RemotePath::make("/usr/lib/libx.so").value();
        EXPECT_EQ(link.resolveLink("libx.so.1")->value(), "/usr/lib/libx.so.1");
        EXPECT_EQ(link.resolveLink("../share/./x/")->value(), "/usr/share/x");
        EXPECT_EQ(link.resolveLink("/opt/x")->value(), "/opt/x");
        EXPECT_EQ(link.resolveLink("../../../../etc")->value(), "/etc");
        EXPECT_EQ(link.resolveLink("..")->value(), "/usr");
        EXPECT_EQ(link.resolveLink("").error().reason, PathRejection::Empty);
        EXPECT_FALSE(RemotePath{}.resolveLink("x").has_value());
    }

    TEST(RemotePathTests, DefaultIsUnset)
    {
        const RemotePath unset{};
        EXPECT_TRUE(unset.empty());
        EXPECT_FALSE(unset.isRoot());
        EXPECT_FALSE(unset.parent().has_value());
        EXPECT_FALSE(unset.isWithin(RemotePath::root()));
        EXPECT_FALSE(RemotePath::root().isWithin(unset));
    }
}
