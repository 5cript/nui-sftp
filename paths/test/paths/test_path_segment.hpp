#pragma once

#include <paths/paths.hpp>

#include <gtest/gtest.h>

#include <string>

namespace Paths::Test
{
    TEST(PathSegmentTests, RefusesNamesThatAreNotASingleSegment)
    {
        for (auto const& [name, reason] : std::initializer_list<std::pair<std::string, PathRejection>>{
                 {"", PathRejection::Empty},
                 {".", PathRejection::DotOrDotDot},
                 {"..", PathRejection::DotOrDotDot},
                 {"x/..", PathRejection::ContainsSeparator},
                 {"a/b", PathRejection::ContainsSeparator},
                 {"/", PathRejection::ContainsSeparator},
                 {std::string{"a\0b", 3}, PathRejection::ContainsNul},
                 {"\xC3\x28", PathRejection::InvalidUtf8},
                 {"\xED\xA0\x80", PathRejection::InvalidUtf8},
                 {"\xC0\xAF", PathRejection::InvalidUtf8},
             })
        {
            const auto segment = PathSegment::make(name);
            ASSERT_FALSE(segment.has_value()) << name;
            EXPECT_EQ(segment.error().reason, reason) << name;
        }
    }

    TEST(PathSegmentTests, AcceptsNamesThatAreLegalOnServers)
    {
        for (auto const* name : {"a", "...", ".hidden", "a\\b", "C:", "con", "trailing.", "\xC3\xA4\xC3\xB6", "a b"})
            EXPECT_TRUE(PathSegment::make(name).has_value()) << name;
    }

    TEST(LocalPathSegmentTests, PosixAcceptsEveryPathSegment)
    {
        for (auto const* name : {"a\\b", "C:", "con", "trailing.", "a?b"})
            EXPECT_TRUE(LocalPathSegment::make(name, LocalPlatform::Posix).has_value()) << name;
    }

    TEST(LocalPathSegmentTests, WindowsRefusesNamesItCannotStore)
    {
        for (auto const& [name, reason] : std::initializer_list<std::pair<std::string_view, PathRejection>>{
                 {"a\\b", PathRejection::ReservedCharacter},
                 {"..\\..\\x", PathRejection::ReservedCharacter},
                 {"C:", PathRejection::ReservedCharacter},
                 {"file.txt:stream", PathRejection::ReservedCharacter},
                 {"a?b", PathRejection::ReservedCharacter},
                 {"tab\there", PathRejection::ReservedCharacter},
                 {"trailing.", PathRejection::TrailingDotOrSpace},
                 {"trailing ", PathRejection::TrailingDotOrSpace},
                 {"CON", PathRejection::ReservedDeviceName},
                 {"nul.txt", PathRejection::ReservedDeviceName},
                 {"Lpt9", PathRejection::ReservedDeviceName},
                 {"com1.tar.gz", PathRejection::ReservedDeviceName},
             })
        {
            const auto segment = LocalPathSegment::make(name, LocalPlatform::Windows);
            ASSERT_FALSE(segment.has_value()) << name;
            EXPECT_EQ(segment.error().reason, reason) << name;
        }
        for (auto const* name : {"console", "com10", "lpt", "a.b", "nul_"})
            EXPECT_TRUE(LocalPathSegment::make(name, LocalPlatform::Windows).has_value()) << name;
    }

    TEST(RelativePathTests, ParsesAndJoinsSegments)
    {
        const auto path = RelativePath::make("a/b/c.txt");
        ASSERT_TRUE(path.has_value());
        EXPECT_EQ(path->segments().size(), 3u);
        EXPECT_EQ(path->leaf()->value(), "c.txt");
        EXPECT_EQ(path->parent()->value(), "a/b");
        EXPECT_EQ(RelativePath{}.value(), "");
        EXPECT_TRUE(path->isWithin(*RelativePath::make("a")));
        EXPECT_FALSE(path->isWithin(*RelativePath::make("a/bb")));
    }

    TEST(RelativePathTests, RefusesAnythingThatCouldLeaveItsRoot)
    {
        for (auto const* text : {"/a", "..", "a/../b", "a//b", "a/", "./a", "x/.."})
            EXPECT_FALSE(RelativePath::make(text).has_value()) << text;
    }
}
