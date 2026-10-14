#pragma once

#include <backend/sftp/remote_delete_guard.hpp>
#include <utility/localized_message.hpp>
#include "real_server_tests.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace Test
{
    TEST(RemoteDeleteGuardDetailTests, RecognizesWindowsServerPaths)
    {
        using RemoteDeleteGuardDetail::looksLikeWindowsPath;
        EXPECT_TRUE(looksLikeWindowsPath(remote("/C:/Users/me")));
        EXPECT_TRUE(looksLikeWindowsPath(remote("/d:")));
        EXPECT_FALSE(looksLikeWindowsPath(remote("/home/C:")));
        EXPECT_FALSE(looksLikeWindowsPath(remote("/CD:/x")));
        EXPECT_FALSE(looksLikeWindowsPath(remote("/C:x")));
        EXPECT_FALSE(looksLikeWindowsPath(Paths::RemotePath::root()));
    }

    TEST(RemoteDeleteGuardDetailTests, RecognizesPathsOfCaseFoldingServers)
    {
        using RemoteDeleteGuardDetail::looksCaseFolding;
        EXPECT_TRUE(looksCaseFolding(remote("/C:/Users/me")));
        EXPECT_TRUE(looksCaseFolding(remote("/Users/me/project")));
        EXPECT_TRUE(looksCaseFolding(remote("/Users")));
        EXPECT_FALSE(looksCaseFolding(remote("/Usersx/me")));
        EXPECT_FALSE(looksCaseFolding(remote("/home/me")));
        EXPECT_FALSE(looksCaseFolding(remote("/srv/Users")));
        EXPECT_FALSE(looksCaseFolding(Paths::RemotePath::root()));
    }

    TEST(RemoteDeleteGuardDetailTests, WithinIgnoringCaseStopsAtSegments)
    {
        using RemoteDeleteGuardDetail::isWithinIgnoringCase;
        EXPECT_TRUE(isWithinIgnoringCase(remote("/home/Test/a"), remote("/HOME/test")));
        EXPECT_TRUE(isWithinIgnoringCase(remote("/home/test"), remote("/Home/Test")));
        EXPECT_TRUE(isWithinIgnoringCase(remote("/a"), Paths::RemotePath::root()));
        EXPECT_FALSE(isWithinIgnoringCase(remote("/home/tests"), remote("/home/test")));
        EXPECT_FALSE(isWithinIgnoringCase(remote("/home"), remote("/home/test")));
        EXPECT_FALSE(isWithinIgnoringCase(Paths::RemotePath{}, Paths::RemotePath::root()));
    }

    TEST(RemoteDeleteGuardDetailTests, TextChecksRefuseRootHomeCaseAndWindowsNames)
    {
        using Reason = RemoteDeleteRefusal::Reason;
        using RemoteDeleteGuardDetail::refusalByText;
        const auto reasonOf = [](Paths::RemotePath const& target, std::optional<Paths::RemotePath> const& home)
        {
            const auto refusal = refusalByText(target, home);
            return refusal ? std::optional{refusal->reason} : std::nullopt;
        };
        const auto home = remote("/home/test");
        EXPECT_EQ(reasonOf(Paths::RemotePath{}, home), Reason::NoPath);
        EXPECT_EQ(reasonOf(Paths::RemotePath::root(), std::nullopt), Reason::Root);
        EXPECT_EQ(reasonOf(remote("/home"), home), Reason::ContainsHome);
        EXPECT_EQ(reasonOf(remote("/home/test"), home), Reason::ContainsHome);
        EXPECT_FALSE(reasonOf(remote("/HOME"), home).has_value()) << "a Linux server tells names apart by case";
        EXPECT_FALSE(reasonOf(remote("/home/Test"), home).has_value());
        EXPECT_FALSE(reasonOf(remote("/home/test/a"), home).has_value());
        EXPECT_FALSE(reasonOf(remote("/home/testing"), home).has_value());
        EXPECT_FALSE(reasonOf(remote("/home"), std::nullopt).has_value());
        EXPECT_FALSE(reasonOf(remote("/home/a\\b"), home).has_value()) << "a backslash is a character on POSIX";

        const auto windowsHome = remote("/C:/Users/me");
        EXPECT_EQ(reasonOf(remote("/c:/users"), windowsHome), Reason::ContainsHome);
        EXPECT_EQ(reasonOf(remote("/C:/USERS/ME"), windowsHome), Reason::ContainsHome);
        EXPECT_EQ(reasonOf(remote("/C:/Users."), windowsHome), Reason::WindowsName);
        EXPECT_EQ(reasonOf(remote("/C:/Users "), windowsHome), Reason::WindowsName);
        EXPECT_EQ(reasonOf(remote("/C:/Users\\me"), windowsHome), Reason::WindowsName);
        EXPECT_EQ(reasonOf(remote("/D:/data\\x"), std::nullopt), Reason::WindowsName);
        EXPECT_FALSE(reasonOf(remote("/C:/Users/me/file.txt"), windowsHome).has_value());
        EXPECT_FALSE(reasonOf(remote("/D:/data/a.b"), windowsHome).has_value());

        const auto macHome = remote("/Users/me");
        EXPECT_EQ(reasonOf(remote("/users"), macHome), Reason::ContainsHome);
        EXPECT_EQ(reasonOf(remote("/USERS/ME"), macHome), Reason::ContainsHome);
        EXPECT_FALSE(reasonOf(remote("/users/me/file.txt"), macHome).has_value());
        EXPECT_EQ(reasonOf(remote("/Users/ME"), home), std::nullopt) << "the home of a Linux server is exact";
        EXPECT_EQ(reasonOf(remote("/Users"), remote("/users/me")), Reason::ContainsHome)
            << "a target below /Users looks like a server that folds case";
    }

    TEST(RemoteDeleteGuardDetailTests, RefusalsAreLocalizedAndSayNothingWasDeleted)
    {
        const RemoteDeleteRefusal refusal{
            .reason = RemoteDeleteRefusal::Reason::ContainsHome,
            .target = "/home",
            .home = "/home/test",
        };
        const auto parsed = Utility::parseLocalizedMessage(refusal.toLocalizedMessage());
        EXPECT_EQ(parsed.key, "backend.deleteGuard.containsHome");
        EXPECT_EQ(parsed.arguments, (std::vector<std::string>{"/home", "/home/test"}));
        EXPECT_EQ(refusal.toString(), "'/home' is or contains the home directory '/home/test'");
    }

    class RemoteDeleteGuardTests : public RealServerTests
    {};

    TEST_F(RemoteDeleteGuardTests, ParentsAreCanonicalizedSoSymlinksDoNotHideTheHome)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createSymlink", {{"path", "/alias"}, {"target", "/home"}}));
        ASSERT_TRUE(serverStartResult->command("createSymlink", {{"path", "/home/test/up"}, {"target", ".."}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::optional<Paths::RemotePath> home{};
        RemoteDeleteGuard guard{*sftp, home, 5s};
        const auto reasonOf = [&guard](std::string_view target) -> std::optional<RemoteDeleteRefusal::Reason>
        {
            const auto checked = guard.check(remote(target));
            return checked ? std::nullopt : std::optional{checked.error().reason};
        };

        EXPECT_EQ(reasonOf("/alias/test"), RemoteDeleteRefusal::Reason::ContainsHome);
        ASSERT_TRUE(home.has_value()) << "the home is looked up once and shared";
        EXPECT_EQ(home->value(), "/home/test");
        EXPECT_EQ(reasonOf("/home/test/up/test"), RemoteDeleteRefusal::Reason::ContainsHome);
        EXPECT_FALSE(reasonOf("/HOME").has_value()) << "the mock server tells names apart by case";
        EXPECT_EQ(reasonOf("/"), RemoteDeleteRefusal::Reason::Root);
        EXPECT_FALSE(reasonOf("/alias/test/file2.txt").has_value());
        EXPECT_FALSE(reasonOf("/alias").has_value()) << "deleting the link removes the link";
        EXPECT_FALSE(reasonOf("/home/test/up").has_value());
        EXPECT_FALSE(reasonOf("/missing/directory/x").has_value());

        const std::vector targets{remote("/home/test/file2.txt"), remote("/alias/test"), remote("/tmp")};
        const auto refusal = refuseAnyProtected(guard, targets);
        ASSERT_TRUE(refusal.has_value());
        EXPECT_EQ(refusal->target, "/alias/test");
        EXPECT_FALSE(refuseAnyProtected(guard, std::vector{remote("/home/test/file2.txt")}).has_value());
    }

    TEST_F(RemoteDeleteGuardTests, ScannedRootsAreCanonicalizedOnce)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createSymlink", {{"path", "/alias"}, {"target", "/home"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::optional<Paths::RemotePath> home{};
        RemoteDeleteGuard guard{*sftp, home, 5s};
        guard.trustScannedBelow(remote("/alias"));
        EXPECT_FALSE(guard.check(remote("/alias/test")).has_value());
        EXPECT_TRUE(guard.check(remote("/alias/test/Documents/notThere")).has_value());
        EXPECT_TRUE(guard.check(remote("/alias/other/x")).has_value());
    }

    TEST_F(RemoteDeleteGuardTests, AnUnknownHomeOnlyProtectsTheRoot)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        serverStartResult->command("realpathDotFailure");
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::optional<Paths::RemotePath> home{};
        RemoteDeleteGuard guard{*sftp, home, 5s};
        EXPECT_EQ(guard.check(Paths::RemotePath::root()).error().reason, RemoteDeleteRefusal::Reason::Root);
        EXPECT_TRUE(guard.check(remote("/home")).has_value());
        EXPECT_FALSE(home.has_value());
    }
}
