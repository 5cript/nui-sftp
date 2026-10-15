#pragma once

#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/delete_operation.hpp>
#include <backend/sftp/local_delete_operation.hpp>
#include "test_transfer_failures.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace Test
{
    class ComparedStateCheckTests : public TransferFailureTests
    {
      public:
        static SharedData::ComparedState localNow(std::filesystem::path const& path)
        {
            return ComparedStateCheck::localStateOf(path).value();
        }

        static SharedData::ComparedState remoteNow(SecureShell::SftpSession& sftp, std::string const& path)
        {
            return sftp
                .performPromise(
                    [&sftp, path = remote(path)]()
                    {
                        return ComparedStateCheck::remoteStateOfInStrand(sftp, path);
                    }
                )
                .get()
                .value();
        }

        static std::expected<void, Operation::Error> checkRemote(
            SecureShell::SftpSession& sftp,
            std::string const& path,
            SharedData::ComparedState const& compared
        )
        {
            return sftp
                .performPromise(
                    [&sftp, path = remote(path), &compared]()
                    {
                        return ComparedStateCheck::requireUnchangedRemoteInStrand(sftp, path, compared);
                    }
                )
                .get();
        }

        static void touchLater(std::filesystem::path const& path)
        {
            std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::hours{1});
        }
    };

    TEST_F(ComparedStateCheckTests, LocalStatesDescribeFilesDirectoriesAndAbsence)
    {
        const auto file = localDirectory_.path() / "file.txt";
        writeLocalFile(file, "12345");
        const auto state = localNow(file);
        EXPECT_TRUE(state.exists);
        EXPECT_EQ(state.type, SharedData::FileType::Regular);
        EXPECT_EQ(state.size, 5u);
        EXPECT_EQ(state.mtime, ComparedStateCheck::secondsSinceEpoch(std::filesystem::last_write_time(file)));

        EXPECT_EQ(localNow(localDirectory_.path()).type, SharedData::FileType::Directory);
        EXPECT_FALSE(localNow(localDirectory_.path() / "missing").exists);
        EXPECT_FALSE(localNow(localDirectory_.path() / "missing" / "below").exists);
    }

#ifndef _WIN32
    TEST_F(ComparedStateCheckTests, ALocalSymlinkIsDescribedByItsLiteral)
    {
        const auto link = localDirectory_.path() / "link";
        std::filesystem::create_symlink("does/not/exist", link);
        const auto state = localNow(link);
        EXPECT_EQ(state.type, SharedData::FileType::Symlink);
        EXPECT_EQ(state.linkTarget, "does/not/exist");

        const auto compared = state;
        std::filesystem::remove(link);
        std::filesystem::create_symlink("elsewhere", link);
        const auto checked = ComparedStateCheck::requireUnchangedLocal(link, compared);
        ASSERT_FALSE(checked);
        EXPECT_EQ(checked.error().type, Operation::ErrorType::ChangedSinceCompare);
    }
#endif

    TEST_F(ComparedStateCheckTests, TheLocalCheckPassesOnlyWhatTheCompareSaw)
    {
        const auto file = localDirectory_.path() / "file.txt";
        writeLocalFile(file, "12345");
        const auto compared = localNow(file);
        EXPECT_TRUE(ComparedStateCheck::requireUnchangedLocal(file, compared)) << "unchanged";
        EXPECT_TRUE(ComparedStateCheck::requireUnchangedLocal(file, std::nullopt)) << "nothing to check";

        const auto expectChanged = [&](SharedData::ComparedState const& expected, std::string const& what)
        {
            const auto checked = ComparedStateCheck::requireUnchangedLocal(file, expected);
            ASSERT_FALSE(checked) << what;
            EXPECT_EQ(checked.error().type, Operation::ErrorType::ChangedSinceCompare) << what;
        };
        const auto time = std::filesystem::last_write_time(file);
        writeLocalFile(file, "123456");
        std::filesystem::last_write_time(file, time);
        expectChanged(compared, "another size");

        writeLocalFile(file, "12345");
        std::filesystem::last_write_time(file, time);
        EXPECT_TRUE(ComparedStateCheck::requireUnchangedLocal(file, compared)) << "restored";
        touchLater(file);
        expectChanged(compared, "another time");

        expectChanged(SharedData::ComparedState{}, "appeared where the compare saw nothing");
        std::filesystem::remove(file);
        EXPECT_TRUE(ComparedStateCheck::requireUnchangedLocal(file, compared)) << "gone loses nothing";
        std::filesystem::create_directory(file);
        expectChanged(compared, "replaced by a directory");
    }

    TEST_F(ComparedStateCheckTests, TheRemoteCheckPassesOnlyWhatTheCompareSaw)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createSymlink", {{"path", "/home/test/link"}, {"target", "file1.txt"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto compared = remoteNow(*sftp, "/home/test/file1.txt");
        EXPECT_EQ(compared.type, SharedData::FileType::Regular);
        EXPECT_EQ(compared.size, std::string{"Fake file content"}.size());
        EXPECT_TRUE(checkRemote(*sftp, "/home/test/file1.txt", compared));

        const auto link = remoteNow(*sftp, "/home/test/link");
        EXPECT_EQ(link.type, SharedData::FileType::Symlink);
        EXPECT_EQ(link.linkTarget, "file1.txt");
        EXPECT_FALSE(remoteNow(*sftp, "/home/test/missing").exists);

        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/file1.txt"}, {"content", "changed"}}));
        const auto changed = checkRemote(*sftp, "/home/test/file1.txt", compared);
        ASSERT_FALSE(changed);
        EXPECT_EQ(changed.error().type, Operation::ErrorType::ChangedSinceCompare);

        const auto appeared = checkRemote(*sftp, "/home/test/file1.txt", SharedData::ComparedState{});
        ASSERT_FALSE(appeared);
        EXPECT_EQ(appeared.error().type, Operation::ErrorType::ChangedSinceCompare);
        EXPECT_TRUE(checkRemote(*sftp, "/home/test/missing", compared)) << "gone loses nothing";
    }

    TEST_F(ComparedStateCheckTests, ADownloadNeverReplacesADestinationThatChangedAfterTheCompare)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto localPath = localDirectory_.path() / "file1.txt";
        writeLocalFile(localPath, "old local");
        const auto compared = localNow(localPath);
        writeLocalFile(localPath, "edited locally meanwhile");

        DownloadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .comparedDestination = compared,
            }
        };
        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::ChangedSinceCompare);
        EXPECT_EQ(readLocalFile(localPath), "edited locally meanwhile");
    }

    TEST_F(ComparedStateCheckTests, ADownloadReplacesADestinationThatIsAsTheCompareSawIt)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto localPath = localDirectory_.path() / "file1.txt";
        writeLocalFile(localPath, "old local");

        DownloadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .comparedDestination = localNow(localPath),
            }
        };
        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(ComparedStateCheckTests, AnUploadNeverReplacesADestinationThatChangedAfterTheCompare)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto localPath = localDirectory_.path() / "upload.txt";
        writeLocalFile(localPath, "new content");
        const auto compared = remoteNow(*sftp, "/home/test/file1.txt");
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/file1.txt"}, {"content", "edited"}}));

        UploadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .comparedDestination = compared,
            }
        };
        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::ChangedSinceCompare);
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "edited");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt.filepart"), "<missing>") << "the temp file is removed";
    }

    TEST_F(ComparedStateCheckTests, AnUploadReplacesADestinationThatIsAsTheCompareSawIt)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto localPath = localDirectory_.path() / "upload.txt";
        writeLocalFile(localPath, "new content");

        UploadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .comparedDestination = remoteNow(*sftp, "/home/test/file1.txt"),
            }
        };
        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "new content");
    }

    TEST_F(ComparedStateCheckTests, ALocalDeleteLeavesATargetThatChangedAfterTheCompare)
    {
        const auto changed = localDirectory_.path() / "changed.txt";
        const auto unchanged = localDirectory_.path() / "unchanged.txt";
        writeLocalFile(changed, "a");
        writeLocalFile(unchanged, "b");
        const auto comparedChanged = localNow(changed);
        const auto comparedUnchanged = localNow(unchanged);
        writeLocalFile(changed, "edited");

        LocalDeleteOperation operation{{
            .targets = {toLocal(changed), toLocal(unchanged)},
            .comparedStates = {comparedChanged, comparedUnchanged},
        }};
        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_TRUE(std::filesystem::exists(changed));
        EXPECT_FALSE(std::filesystem::exists(unchanged));
        ASSERT_EQ(operation.getFailed().size(), 1u);
        EXPECT_EQ(operation.getFailed().front().second.type, Operation::ErrorType::ChangedSinceCompare);
    }

    TEST_F(ComparedStateCheckTests, ARemoteDeleteLeavesATargetThatChangedAfterTheCompare)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/changed.txt"}, {"content", "a"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/unchanged.txt"}, {"content", "b"}}));
        ASSERT_TRUE(serverStartResult->command("createSymlink", {{"path", "/home/test/link"}, {"target", "a"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        const auto comparedChanged = remoteNow(*sftp, "/home/test/changed.txt");
        const auto comparedUnchanged = remoteNow(*sftp, "/home/test/unchanged.txt");
        auto comparedLink = remoteNow(*sftp, "/home/test/link");
        comparedLink.linkTarget = "b";
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/changed.txt"}, {"content", "edited"}}));

        DeleteOperation operation{sftp, {.recursive = true}};
        operation.setTargets({
            {.path = remote("/home/test/changed.txt"), .compared = comparedChanged},
            {.path = remote("/home/test/unchanged.txt"), .compared = comparedUnchanged},
            {.path = remote("/home/test/link"), .compared = comparedLink},
            {.path = remote("/home/test/gone.txt"), .compared = comparedUnchanged},
        });
        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/changed.txt"), "edited");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/unchanged.txt"), "<missing>");
        EXPECT_TRUE(remoteNow(*sftp, "/home/test/link").exists);
        ASSERT_EQ(operation.getFailed().size(), 2u) << "a gone target counts as deleted";
        for (auto const& [path, error] : operation.getFailed())
            EXPECT_EQ(error.type, Operation::ErrorType::ChangedSinceCompare) << path;
    }
}
