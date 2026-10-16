#pragma once

#include <backend/existence_check.hpp>
#include "real_server_tests.hpp"

#include <utility/temporary_directory.hpp>
#include <paths/native.hpp>

#include <nui/utility/scope_exit.hpp>

#ifndef _WIN32
#    include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class ExistenceCheckTests : public RealServerTests
    {
      protected:
        static constexpr std::chrono::milliseconds timeout{10'000};

        std::string localPath(std::filesystem::path const& relative) const
        {
            return Paths::nativeToUtf8(root_.path() / relative);
        }

        Utility::TemporaryDirectory root_{temporaryRoot, false};
    };

    TEST_F(ExistenceCheckTests, LocalMissingPathIsAbsentAndExistingPathsExist)
    {
        std::ofstream{root_.path() / "file.txt"} << "content";
        std::filesystem::create_directory(root_.path() / "directory");

        const auto result =
            localPathsExist({localPath("file.txt"), localPath("directory"), localPath("missing.txt"), localPath("no/x")});

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, (std::vector<bool>{true, true, false, false}));
    }

    TEST_F(ExistenceCheckTests, LocalDanglingSymlinkExists)
    {
        std::error_code errorCode{};
        std::filesystem::create_symlink(root_.path() / "nowhere.txt", root_.path() / "dangling", errorCode);
        if (errorCode)
            GTEST_SKIP() << "cannot create symlinks here: " << errorCode.message();

        const auto result = localPathsExist({localPath("dangling")});

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, (std::vector<bool>{true}));
    }

#ifndef _WIN32
    TEST_F(ExistenceCheckTests, LocalPathThatCannotBeCheckedFailsTheCheck)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can look into any directory";
        const auto locked = root_.path() / "locked";
        std::filesystem::create_directory(locked);
        std::ofstream{locked / "inside.txt"} << "content";
        std::filesystem::permissions(locked, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&locked]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        const auto result = localPathsExist({localPath("missing.txt"), localPath("locked/inside.txt")});

        EXPECT_FALSE(result.has_value()) << "an unknown destination must never count as absent";
    }
#endif

    TEST_F(ExistenceCheckTests, RemoteFewPathsAreCheckedOneByOne)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto result = remotePathsExist(
            *sftp,
            {"/home/test/file1.txt", "/home/test/dangling_link", "/home/test/missing.txt", "/home/test/no/x"},
            timeout
        );

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, (std::vector<bool>{true, true, false, false}));
    }

    TEST_F(ExistenceCheckTests, RemoteStatFailureFailsTheCheck)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ASSERT_TRUE(serverStartResult->command("denyStat", {{"path", "/home/test/file2.txt"}}));

        const auto result = remotePathsExist(*sftp, {"/home/test/missing.txt", "/home/test/file2.txt"}, timeout);

        EXPECT_FALSE(result.has_value()) << "an unknown destination must never count as absent";
    }

    TEST_F(ExistenceCheckTests, RemoteManyPathsAreCheckedByListing)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::vector<std::string> paths{
            "/home/test/file1.txt",
            "/home/test/dangling_link",
            "/home/test/Documents",
            "/home/test/Documents/doc1.txt",
            "/home/test/missing/x.txt",
        };
        std::vector<bool> expected{true, true, true, true, false};
        for (int index = 0; index < 8; ++index)
        {
            paths.push_back("/home/test/missing" + std::to_string(index) + ".txt");
            expected.push_back(false);
        }

        const auto result = remotePathsExist(*sftp, paths, timeout);

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, expected);
    }

    TEST_F(ExistenceCheckTests, RemoteDirectoryThatCannotBeListedIsCheckedPathByPath)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        // A drop box: writable, but not listable.
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));

        std::vector<std::string> paths{"/home/test/Documents/doc1.txt"};
        std::vector<bool> expected{true};
        for (int index = 0; index < 12; ++index)
        {
            paths.push_back("/home/test/Documents/new" + std::to_string(index) + ".txt");
            expected.push_back(false);
        }

        const auto result = remotePathsExist(*sftp, paths, timeout);

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, expected);
    }

    TEST_F(ExistenceCheckTests, RemoteNameListedOnlyInAnotherCaseIsLookedUp)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        // The lookup of the variant fails here, which shows it happens: a listed exact name needs none.
        ASSERT_TRUE(serverStartResult->command("denyStat", {{"path", "/home/test/FILE1.txt"}}));

        std::vector<std::string> paths{"/home/test/file1.txt"};
        for (int index = 0; index < 10; ++index)
            paths.push_back("/home/test/new" + std::to_string(index) + ".txt");
        const auto exact = remotePathsExist(*sftp, paths, timeout);
        ASSERT_TRUE(exact.has_value()) << exact.error();
        EXPECT_TRUE(exact->front());

        paths.front() = "/home/test/FILE1.txt";
        EXPECT_FALSE(remotePathsExist(*sftp, paths, timeout).has_value())
            << "the server may fold case, so the variant is looked up instead of taken for absent";
    }

    TEST_F(ExistenceCheckTests, RemoteFewChildrenOfAParentAreStatEvenAmongManyPaths)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        // Listing it would fail, a stat of its one queried child does not.
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Pictures"}}));

        std::vector<std::string> paths{"/home/test/Pictures/image1.png"};
        std::vector<bool> expected{true};
        for (int index = 0; index < 12; ++index)
        {
            paths.push_back("/home/test/new" + std::to_string(index) + ".txt");
            expected.push_back(false);
        }

        const auto result = remotePathsExist(*sftp, paths, timeout);

        ASSERT_TRUE(result.has_value()) << result.error();
        EXPECT_EQ(*result, expected);
    }

    TEST_F(ExistenceCheckTests, RemoteInvalidPathFailsTheCheck)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        EXPECT_FALSE(remotePathsExist(*sftp, {"relative.txt"}, timeout).has_value());
        EXPECT_FALSE(remotePathsExist(*sftp, {"/home/test/../test/file1.txt"}, timeout).has_value());
    }
}
