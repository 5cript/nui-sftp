#pragma once

#include <utility/permissions.hpp>

#include <gtest/gtest.h>

#include <filesystem>

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
}
