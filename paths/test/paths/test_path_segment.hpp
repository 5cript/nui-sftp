#pragma once

#include <paths/paths.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

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

    TEST(Utf8Tests, AcceptsOnlyWellFormedSequences)
    {
        for (auto const* text : {"", "ascii", "\xC3\xA4", "\xE2\x82\xAC", "\xF0\x9F\x9A\x80", "\xF4\x8F\xBF\xBF"})
            EXPECT_TRUE(isValidUtf8(text)) << text;
        for (auto const* text : {
                 "\x80",
                 "\xBF",
                 "\xC3",
                 "\xE2\x82",
                 "\xF0\x9F\x9A",
                 "\xF4\x90\x80\x80",
                 "\xF5\x80\x80\x80",
                 "\xF8\x88\x80\x80\x80",
                 "\xFC\x84\x80\x80\x80\x80",
                 "\xFF",
                 "\xE0\x80\xAF",
                 "\xF0\x80\x80\xAF",
                 "\xED\xBF\xBF",
                 "a\xC3(b",
             })
            EXPECT_FALSE(isValidUtf8(text)) << text;
    }

    TEST(Utf8Tests, CountsUtf16Units)
    {
        EXPECT_EQ(utf16Length(""), 0u);
        EXPECT_EQ(utf16Length("abc"), 3u);
        EXPECT_EQ(utf16Length("\xC3\xA4\xE2\x82\xAC"), 2u);
        EXPECT_EQ(utf16Length("\xF0\x9F\x9A\x80"), 2u);
    }

    TEST(PathErrorTests, EscapesControlBytesAndInvalidUtf8)
    {
        const auto printable = [](std::string input)
        {
            return PathError{.reason = PathRejection::InvalidUtf8, .input = std::move(input)}.toString();
        };
        EXPECT_EQ(printable("plain"), "'plain' is not valid UTF-8");
        EXPECT_EQ(printable("tab\there\x7F"), "'tab\\x09here\\x7F' is not valid UTF-8");
        EXPECT_EQ(printable(std::string{"a\0b", 3}), "'a\\x00b' is not valid UTF-8");
        EXPECT_EQ(printable("Gr\xC3\xBC\xC3\x9F\xF0\x9F\x9A\x80"), "'Gr\xC3\xBC\xC3\x9F\xF0\x9F\x9A\x80' is not valid UTF-8");
        EXPECT_EQ(printable("a\xC3(\xFF\xE2\x82"), "'a\\xC3(\\xFF\\xE2\\x82' is not valid UTF-8");
        EXPECT_EQ(printable("\xED\xA0\x80"), "'\\xED\\xA0\\x80' is not valid UTF-8");
        EXPECT_TRUE(isValidUtf8(printable("\xC0\xAF\xF8\x88\x80\x80\x80")));
    }

    TEST(RelativePathTests, ParsesAndJoinsSegments)
    {
        const auto path = RelativePath::make("a/b/c.txt");
        ASSERT_TRUE(path.has_value());
        ASSERT_EQ(path->segments().size(), 3u);
        EXPECT_EQ(path->segments()[1].value(), "b");
        auto views = path->segmentViews();
        EXPECT_EQ(
            std::vector<std::string_view>(views.begin(), views.end()), (std::vector<std::string_view>{"a", "b", "c.txt"})
        );
        EXPECT_TRUE(RelativePath{}.segments().empty());
        EXPECT_EQ((RelativePath{} / PathSegment::make("x").value()).value(), "x");
        EXPECT_EQ((*path / RelativePath{}).value(), "a/b/c.txt");
        EXPECT_EQ((RelativePath{} / *path).value(), "a/b/c.txt");
        EXPECT_EQ((*RelativePath::make("a") / *RelativePath::make("b/c")).value(), "a/b/c");
        EXPECT_FALSE(RelativePath{}.parent().has_value());
        EXPECT_EQ(RelativePath::make("a")->parent()->value(), "");
        EXPECT_EQ(path->leaf()->value(), "c.txt");
        EXPECT_EQ(path->parent()->value(), "a/b");
        EXPECT_EQ(RelativePath{}.value(), "");
        EXPECT_TRUE(path->isWithin(*RelativePath::make("a")));
        EXPECT_FALSE(path->isWithin(*RelativePath::make("a/bb")));
    }

    TEST(RelativePathTests, RefusesAnythingThatCouldLeaveItsRoot)
    {
        for (auto const* text : {"/a", "..", "a/../b", "a//b", "a/", "./a", "x/..", "a/\xFF"})
            EXPECT_FALSE(RelativePath::make(text).has_value()) << text;
        EXPECT_EQ(RelativePath::make("/a").error().reason, PathRejection::EscapesRoot);
        EXPECT_EQ(RelativePath::make(std::string{"a/b\0c", 5}).error().reason, PathRejection::ContainsNul);
    }

    TEST(PathSplitTests, SplitsAtEverySlash)
    {
        const auto parts = [](std::string_view text)
        {
            auto split = Detail::splitAtSlashes(text);
            return std::vector<std::string_view>(split.begin(), split.end());
        };
        EXPECT_TRUE(parts("").empty());
        EXPECT_EQ(parts("a"), (std::vector<std::string_view>{"a"}));
        EXPECT_EQ(parts("/a//b/"), (std::vector<std::string_view>{"", "a", "", "b", ""}));
    }
}
