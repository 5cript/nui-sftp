#pragma once

#include <backend/local_delete_guard.hpp>

#include <gtest/gtest.h>

namespace Test
{
    class LocalDeleteGuardTests : public ::testing::Test
    {
      protected:
        // POSIX rules on every host, the Windows rules are covered by the paths tests.
        void SetUp() override
        {
            previous_ = Paths::defaultLocalPlatform();
            Paths::defaultLocalPlatform() = Paths::LocalPlatform::Posix;
        }
        void TearDown() override
        {
            Paths::defaultLocalPlatform() = previous_;
        }

      private:
        Paths::LocalPlatform previous_{};
    };

    TEST_F(LocalDeleteGuardTests, RefusesRootsHomeAndItsAncestors)
    {
        const auto home = Paths::LocalPath::fromUtf8("/home/user", Paths::LocalPlatform::Posix);
        ASSERT_TRUE(home.has_value());
        const auto check = [&home](std::string const& path)
        {
            return localDeleteTarget(path, *home).has_value();
        };
        EXPECT_FALSE(check("/"));
        EXPECT_FALSE(check("/home"));
        EXPECT_FALSE(check("/home/user"));
        EXPECT_FALSE(check("/home/user/../other"));
        EXPECT_FALSE(check("relative/path"));
        EXPECT_FALSE(check(""));
        EXPECT_TRUE(check("/home/user/file.txt"));
        EXPECT_TRUE(check("/home/username"));
        EXPECT_TRUE(check("/tmp/x"));
    }

    TEST_F(LocalDeleteGuardTests, WindowsRootsHomeAndBackslashParentsAreRefused)
    {
        Paths::defaultLocalPlatform() = Paths::LocalPlatform::Windows;
        const auto home = Paths::LocalPath::fromUtf8("C:/Users/me");
        ASSERT_TRUE(home.has_value());
        const auto check = [&home](std::string const& path)
        {
            return localDeleteTarget(path, *home).has_value();
        };
        EXPECT_FALSE(check("C:/"));
        EXPECT_FALSE(check("C:/Users"));
        EXPECT_FALSE(check("C:\\Users\\me"));
        EXPECT_FALSE(check("C:\\Users\\me\\..\\other"));
        EXPECT_FALSE(check("/no/drive"));
        EXPECT_TRUE(check("C:/Users/me/file.txt"));
        EXPECT_TRUE(check("D:/data"));
    }

    TEST_F(LocalDeleteGuardTests, WithoutAKnownHomeOnlyRootsAreRefused)
    {
        EXPECT_FALSE(localDeleteTarget("/", std::nullopt).has_value());
        EXPECT_TRUE(localDeleteTarget("/home/user", std::nullopt).has_value());
    }
}
