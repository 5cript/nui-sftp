#pragma once

#include "test_side_paths.hpp"

#include <frontend/file_explorer/name_rejection.hpp>
#include <frontend/file_explorer/side_paths.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace FileExplorer::Test
{
    namespace ItemNamesTestDetail
    {
        inline std::optional<Paths::PathRejection> remoteRejection(std::string_view name)
        {
            const auto child = childOf(*Paths::RemotePath::make("/home/user"), name);
            if (child)
                return std::nullopt;
            return child.error().reason;
        }

        inline std::optional<Paths::PathRejection> localRejection(std::string_view name, Paths::LocalPlatform platform)
        {
            const SidePathsTestDetail::LocalPlatformScope scope{platform};
            const auto directory =
                Paths::LocalPath::fromUtf8(platform == Paths::LocalPlatform::Windows ? "C:/Users" : "/home/user");
            const auto child = childOf(*directory, name);
            if (child)
                return std::nullopt;
            return child.error().reason;
        }
    }

    TEST(ItemNamesTests, ARemoteNameMayContainABackslashOrColon)
    {
        using namespace ItemNamesTestDetail;
        EXPECT_EQ(remoteRejection("a\\b"), std::nullopt);
        EXPECT_EQ(remoteRejection("a:b"), std::nullopt);
        EXPECT_EQ(remoteRejection("CON"), std::nullopt);
    }

    TEST(ItemNamesTests, ARemoteNameIsOneSegment)
    {
        using namespace ItemNamesTestDetail;
        EXPECT_EQ(remoteRejection(""), Paths::PathRejection::Empty);
        EXPECT_EQ(remoteRejection(".."), Paths::PathRejection::DotOrDotDot);
        EXPECT_EQ(remoteRejection("a/b"), Paths::PathRejection::ContainsSeparator);
        EXPECT_EQ(remoteRejection(std::string_view{"a\0b", 3}), Paths::PathRejection::ContainsNul);
        EXPECT_EQ(remoteRejection("\xFF"), Paths::PathRejection::InvalidUtf8);
    }

    TEST(ItemNamesTests, APosixLocalNameMayContainABackslash)
    {
        using namespace ItemNamesTestDetail;
        EXPECT_EQ(localRejection("a\\b", Paths::LocalPlatform::Posix), std::nullopt);
        EXPECT_EQ(localRejection("name.", Paths::LocalPlatform::Posix), std::nullopt);
        EXPECT_EQ(localRejection("a/b", Paths::LocalPlatform::Posix), Paths::PathRejection::ContainsSeparator);
    }

    TEST(ItemNamesTests, AWindowsLocalNameFollowsTheWindowsRules)
    {
        using namespace ItemNamesTestDetail;
        EXPECT_EQ(localRejection("a\\b", Paths::LocalPlatform::Windows), Paths::PathRejection::ReservedCharacter);
        EXPECT_EQ(localRejection("a:b", Paths::LocalPlatform::Windows), Paths::PathRejection::ReservedCharacter);
        EXPECT_EQ(localRejection("nul.txt", Paths::LocalPlatform::Windows), Paths::PathRejection::ReservedDeviceName);
        EXPECT_EQ(localRejection("name.", Paths::LocalPlatform::Windows), Paths::PathRejection::TrailingDotOrSpace);
        EXPECT_EQ(
            localRejection(std::string(256, 'x'), Paths::LocalPlatform::Windows), Paths::PathRejection::NameTooLong
        );
        EXPECT_EQ(localRejection("report.txt", Paths::LocalPlatform::Windows), std::nullopt);
    }

    TEST(ItemNamesTests, EveryNameRejectionHasItsOwnExplanation)
    {
        const std::set<std::string_view> keys{
            nameRejectionKey(Paths::PathRejection::Empty),
            nameRejectionKey(Paths::PathRejection::DotOrDotDot),
            nameRejectionKey(Paths::PathRejection::ContainsSeparator),
            nameRejectionKey(Paths::PathRejection::ContainsNul),
            nameRejectionKey(Paths::PathRejection::InvalidUtf8),
            nameRejectionKey(Paths::PathRejection::ReservedCharacter),
            nameRejectionKey(Paths::PathRejection::ReservedDeviceName),
            nameRejectionKey(Paths::PathRejection::TrailingDotOrSpace),
            nameRejectionKey(Paths::PathRejection::NameTooLong),
        };
        EXPECT_EQ(keys.size(), 9u);
        EXPECT_FALSE(keys.contains("Other"));
    }

    TEST(ItemNamesTests, RejectionsOfWholePathsShareTheGeneralExplanation)
    {
        EXPECT_EQ(std::string_view{nameRejectionKey(Paths::PathRejection::NotAbsolute)}, "Other");
        EXPECT_EQ(std::string_view{nameRejectionKey(Paths::PathRejection::EscapesRoot)}, "Other");
        EXPECT_EQ(std::string_view{nameRejectionKey(Paths::PathRejection::DevicePath)}, "Other");
        EXPECT_EQ(std::string_view{nameRejectionKey(Paths::PathRejection::UnknownParent)}, "Other");
    }
}
