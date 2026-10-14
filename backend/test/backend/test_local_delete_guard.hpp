#pragma once

#include <backend/local_delete_guard.hpp>
#include <utility/temporary_directory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class LocalDeleteGuardTests : public ::testing::Test
    {
      protected:
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
            return localDeleteTarget(path, {*home}).has_value();
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
        EXPECT_TRUE(check("/tmp/a\\..\\b")) << "a backslash is a character on POSIX";
    }

    TEST_F(LocalDeleteGuardTests, EveryHomeIsProtected)
    {
        const std::vector homes{
            Paths::LocalPath::fromUtf8("/home/user", Paths::LocalPlatform::Posix).value(),
            Paths::LocalPath::fromUtf8("/var/home/user", Paths::LocalPlatform::Posix).value(),
        };
        EXPECT_FALSE(localDeleteTarget("/var/home", homes).has_value());
        EXPECT_FALSE(localDeleteTarget("/home", homes).has_value());
        EXPECT_TRUE(localDeleteTarget("/var/tmp", homes).has_value());
    }

    TEST_F(LocalDeleteGuardTests, WindowsRootsHomeAndBackslashParentsAreRefused)
    {
        Paths::defaultLocalPlatform() = Paths::LocalPlatform::Windows;
        const auto home = Paths::LocalPath::fromUtf8("C:/Users/me");
        ASSERT_TRUE(home.has_value());
        const auto check = [&home](std::string const& path)
        {
            return localDeleteTarget(path, {*home}).has_value();
        };
        EXPECT_FALSE(check("C:/"));
        EXPECT_FALSE(check("C:/Users"));
        EXPECT_FALSE(check("C:\\Users\\me"));
        EXPECT_FALSE(check("C:\\Users\\me\\..\\other"));
        EXPECT_FALSE(check("/no/drive"));
        EXPECT_TRUE(check("C:/Users/me/file.txt"));
        EXPECT_TRUE(check("D:/data"));
    }

    TEST_F(LocalDeleteGuardTests, WindowsSpellingsOfTheHomeAreRefused)
    {
        Paths::defaultLocalPlatform() = Paths::LocalPlatform::Windows;
        const auto home = Paths::LocalPath::fromUtf8("C:/Users/me").value();
        for (auto const* path : {"c:/users", "C:/USERS/ME", "\\\\?\\C:\\Users", "\\\\?\\c:\\users\\Me", "c:\\"})
            EXPECT_FALSE(localDeleteTarget(path, {home}).has_value()) << path;
        EXPECT_EQ(localDeleteTarget("\\\\.\\C:\\Users", {home}).error(), "'\\\\.\\C:\\Users' is a device path");
        EXPECT_TRUE(localDeleteTarget("c:/users/me/x", {home}).has_value());
        EXPECT_TRUE(localDeleteTarget("c:/userss", {home}).has_value());
    }

    TEST_F(LocalDeleteGuardTests, RefusalsNameTheReason)
    {
        const auto home = Paths::LocalPath::fromUtf8("/home/user", Paths::LocalPlatform::Posix).value();
        EXPECT_EQ(localDeleteTarget("relative", {home}).error(), "'relative' is not absolute");
        EXPECT_EQ(localDeleteTarget("/a/\xFF", {home}).error(), "'/a/\\xFF' is not valid UTF-8");
        EXPECT_EQ(localDeleteTarget("/a/../b", {home}).error(), "'/a/../b' contains '..'");
        EXPECT_EQ(localDeleteTarget("/", {home}).error(), "'/' is a root directory");
        EXPECT_EQ(
            localDeleteTarget("/home", {home}).error(), "'/home' is or contains the home directory '/home/user'"
        );
    }

    TEST_F(LocalDeleteGuardTests, WithoutAKnownHomeOnlyRootsAreRefused)
    {
        EXPECT_FALSE(localDeleteTarget("/", {}).has_value());
        EXPECT_TRUE(localDeleteTarget("/home/user", {}).has_value());
    }

#ifndef _WIN32
    TEST_F(LocalDeleteGuardTests, TheSameDirectoryThroughASymlinkIsRefused)
    {
        const Utility::TemporaryDirectory directory{temporaryRoot / "localDeleteGuard"};
        const auto root = directory.path();
        std::filesystem::create_directories(root / "real" / "user" / "work");
        std::filesystem::create_directory_symlink(root / "real", root / "link");
        const auto home = Paths::LocalPath::make(root / "link" / "user").value();
        const auto check = [&home](std::filesystem::path const& path)
        {
            return refuseProtectedLocalDelete(Paths::LocalPath::make(path).value(), {home}).has_value();
        };

        EXPECT_FALSE(check(root / "real")) << "an ancestor of the home reached without the link";
        EXPECT_FALSE(check(root / "real" / "user"));
        EXPECT_FALSE(check(root / "link"));
        EXPECT_TRUE(check(root / "real" / "user" / "work"));
        EXPECT_TRUE(check(root / "missing"));

        // Deleting a link removes the link, never what it points to.
        std::filesystem::create_directory_symlink(root / "real" / "user", root / "toHome");
        EXPECT_TRUE(check(root / "toHome"));
    }

    TEST_F(LocalDeleteGuardTests, HomeDirectoriesComeFromTheEnvironmentAndPasswd)
    {
        const auto homes = localHomeDirectories();
        ASSERT_FALSE(homes.empty());
        if (auto const* home = std::getenv("HOME"); home != nullptr && *home != '\0')
            EXPECT_EQ(homes.front().native(), std::filesystem::path{home}.lexically_normal());
        for (auto const& home : homes)
            EXPECT_EQ(std::ranges::count(homes, home), 1);
    }
#endif
}
