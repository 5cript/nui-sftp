#pragma once

#include <utility/permissions.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Utility::Test
{
    using std::filesystem::perms;

    TEST(PermissionsTests, UnknownPermissionsAreNotApplied)
    {
        EXPECT_FALSE(sanitizePermissions(perms::unknown).has_value());
        EXPECT_FALSE(inheritablePermissions(perms::unknown).has_value());
    }

    TEST(PermissionsTests, SpecialBitsNeverTravel)
    {
        const auto sanitized = sanitizePermissions(perms::all | perms::set_uid | perms::set_gid | perms::sticky_bit);
        ASSERT_TRUE(sanitized.has_value());
        EXPECT_EQ(*sanitized, perms::all);
    }

    TEST(PermissionsTests, InheritedPermissionsHonorTheUmask)
    {
        const auto inherited = inheritablePermissions(perms::all);
        ASSERT_TRUE(inherited.has_value());
        EXPECT_EQ(*inherited, perms::all & ~processUmask());
        EXPECT_EQ(*inherited & processUmask(), perms::none);
    }

#ifndef _WIN32
    TEST(PermissionsTests, NewFilePermissionsAreThoseOfACreatedFile)
    {
        const TemporaryDirectory directory{temporaryRoot, true};
        std::ofstream{directory.path() / "created"} << "x";
        EXPECT_EQ(std::filesystem::status(directory.path() / "created").permissions(), newFilePermissions());
    }

    TEST(PermissionsTests, RestrictToOwnerCreatesAPrivateFile)
    {
        const TemporaryDirectory directory{temporaryRoot, true};
        const auto path = directory.path() / "private";
        ASSERT_TRUE(restrictToOwner(path).has_value());
        EXPECT_EQ(std::filesystem::status(path).permissions(), perms::owner_read | perms::owner_write);
    }

    TEST(PermissionsTests, RestrictToOwnerKeepsTheContentOfAnExistingFile)
    {
        const TemporaryDirectory directory{temporaryRoot, true};
        const auto path = directory.path() / "existing";
        std::ofstream{path} << "content";
        std::filesystem::permissions(path, perms::owner_all | perms::group_read | perms::others_read);
        ASSERT_TRUE(restrictToOwner(path).has_value());
        EXPECT_EQ(std::filesystem::status(path).permissions(), perms::owner_read | perms::owner_write);
        std::ifstream file{path};
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}), "content");
    }

    TEST(PermissionsTests, RestrictToOwnerRefusesALink)
    {
        const TemporaryDirectory directory{temporaryRoot, true};
        std::ofstream{directory.path() / "target"} << "x";
        const auto before = std::filesystem::status(directory.path() / "target").permissions();
        std::filesystem::create_symlink(directory.path() / "target", directory.path() / "link");
        EXPECT_FALSE(restrictToOwner(directory.path() / "link").has_value());
        EXPECT_EQ(std::filesystem::status(directory.path() / "target").permissions(), before);
    }
#endif
}
