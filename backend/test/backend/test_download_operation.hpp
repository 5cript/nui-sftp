#pragma once

#include <backend/sftp/download_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>

#include <fstream>
#include <string>

namespace Test
{
    class DownloadOperationTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            RealServerTests::SetUp();
        }

        std::string readLocalFile(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

      protected:
        // Isolated subdirectory per test run so downloaded files do not collide across tests.
        Utility::TemporaryDirectory downloadDir_{programDirectory / "temp", true};
    };

    TEST_F(DownloadOperationTests, CanCreateServer)
    {
        /*self test*/
        CREATE_SERVER_AND_JOINER(sftpServer);
    }

    TEST_F(DownloadOperationTests, CanCreateDownloadOperation)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        DownloadOperation operation{*sftp, {}};
    }

    TEST_F(DownloadOperationTests, DownloadOperationTypeIsDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        DownloadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::Download);
    }

    TEST_F(DownloadOperationTests, CanPrepareDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
            }};

        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());
        EXPECT_TRUE(std::filesystem::exists(localPath.generic_string() + ".filepart"));
    }

    TEST_F(DownloadOperationTests, CanDownloadFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
            }};

        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, FilepartAbsentAfterSuccessfulDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
            }};

        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        EXPECT_FALSE(std::filesystem::exists(localPath.generic_string() + ".filepart"));
    }

    TEST_F(DownloadOperationTests, DownloadFailsWhenLocalFileExists)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";
        {
            std::ofstream existingFile{localPath};
        }

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .mayOverwrite = false,
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, DownloadOperation::ErrorType::FileExists);
    }

    TEST_F(DownloadOperationTests, DownloadOverwritesExistingFileWhenEnabled)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        // First download
        {
            DownloadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/file1.txt",
                    .localPath = localPath,
                }};

            DownloadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == DownloadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);
        }

        // Second download with overwrite enabled
        {
            DownloadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/file1.txt",
                    .localPath = localPath,
                    .mayOverwrite = true,
                }};

            DownloadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == DownloadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);
        }

        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, DownloadNonExistentRemoteFileFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/does_not_exist.txt",
                .localPath = downloadDir_.path() / "does_not_exist.txt",
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, DownloadOperation::ErrorType::FileStatFailed);
    }

    TEST_F(DownloadOperationTests, DownloadToEmptyLocalPathFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = {},
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, DownloadOperation::ErrorType::InvalidPath);
    }

    TEST_F(DownloadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = downloadDir_.path() / "file1.txt",
            }};

        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, DownloadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(DownloadOperationTests, WorkOnFailedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/does_not_exist.txt",
                .localPath = downloadDir_.path() / "does_not_exist.txt",
            }};

        auto firstResult = operation.work();
        ASSERT_FALSE(firstResult.has_value());

        auto secondResult = operation.work();
        ASSERT_FALSE(secondResult.has_value());
        EXPECT_EQ(secondResult.error().type, DownloadOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(DownloadOperationTests, DoCleanupRemovesFilepartOnCancel)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .doCleanup = true,
            }};

        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());
        ASSERT_TRUE(std::filesystem::exists(localPath.generic_string() + ".filepart"));

        ASSERT_TRUE(operation.cancel(true).has_value());
        EXPECT_FALSE(std::filesystem::exists(localPath.generic_string() + ".filepart"));
    }

    TEST_F(DownloadOperationTests, NoCleanupKeepsFilepartOnCancel)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .doCleanup = false,
            }};

        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());
        ASSERT_TRUE(std::filesystem::exists(localPath.generic_string() + ".filepart"));

        ASSERT_TRUE(operation.cancel(true).has_value());
        EXPECT_TRUE(std::filesystem::exists(localPath.generic_string() + ".filepart"));
    }

    TEST_F(DownloadOperationTests, CanContinuePartialDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";
        const auto filepartPath = localPath.generic_string() + ".filepart";

        // Write the first 8 bytes of "Fake file content" to simulate a previous interrupted download
        {
            std::ofstream partial{filepartPath, std::ios::binary};
            partial << "Fake fil";
        }

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .tryContinue = true,
            }};

        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, PartialFileLargerThanRemoteStartsFresh)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";
        const auto filepartPath = localPath.generic_string() + ".filepart";

        // Write more bytes than the remote file size (17) to simulate a corrupted partial download
        {
            std::ofstream partial{filepartPath, std::ios::binary};
            partial << std::string(100, 'x');
        }

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .tryContinue = true,
            }};

        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, LargeFileDownloadSucceeds)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "large.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/large.txt",
                .localPath = localPath,
            }};

        // large.txt is 1 MiB: needs ~64 work() calls with a 16384-byte buffer
        DownloadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 200; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == DownloadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, DownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_EQ(std::filesystem::file_size(localPath), 1024u * 1024u);
    }

    // ---- Symlink handling --------------------------------------------------
    //
    // Remote symlinks must be recreated as local symlinks, not downloaded as files.
    // The fixtures are defined in sftp_server.mjs:
    //   /home/test/link_to_file1.txt -> /home/test/file1.txt   (link to file)
    //   /home/test/link_to_documents -> /home/test/Documents   (link to directory)
    //   /home/test/dangling_link     -> /home/test/does_not_exist.txt

    namespace
    {
        inline DownloadOperation::WorkStatus
        runDownloadToCompletion(DownloadOperation& op, int maxIterations = 100)
        {
            DownloadOperation::WorkStatus last{};
            for (int i = 0; i < maxIterations; ++i)
            {
                auto result = op.work();
                EXPECT_TRUE(result.has_value()) << "work() failed unexpectedly";
                if (!result.has_value())
                    return last;
                last = result.value();
                if (last == DownloadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }
    }

    TEST_F(DownloadOperationTests, DownloadRemoteSymlinkToFileCreatesLocalSymlink)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "link_to_file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/link_to_file1.txt",
                .localPath = localPath,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(localPath)))
            << "expected a local symlink at " << localPath.string();
        // And the link must not have been materialized as a regular file containing the target's bytes.
        EXPECT_FALSE(std::filesystem::is_regular_file(std::filesystem::symlink_status(localPath)));
    }

    TEST_F(DownloadOperationTests, DownloadRemoteSymlinkToDirectoryCreatesLocalSymlink)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "link_to_documents";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/link_to_documents",
                .localPath = localPath,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(localPath)));
        // It must NOT be a real directory full of downloaded content.
        EXPECT_FALSE(std::filesystem::is_directory(std::filesystem::symlink_status(localPath)));
    }

    TEST_F(DownloadOperationTests, DownloadRemoteSymlinkWithSkipSymlinkCreatesNothing)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "skipped_link";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/link_to_file1.txt",
                .localPath = localPath,
                .symlinkHandling = Persistence::SymlinkHandling::SkipSymlink,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);

        EXPECT_FALSE(std::filesystem::exists(std::filesystem::symlink_status(localPath)))
            << "SkipSymlink must not create any local entry";
    }

    // NOTE: A DownloadRemoteSymlinkWithFollowSymlinkDownloadsTargetContent test would need
    // the server's OPEN handler to transparently follow symlinks (ssh2's fake server does
    // not). FollowSymlink isn't part of any sync scenario we hit, so we don't cover it here.

    // ---- createMissingDirectories option ----------------------------------
    //
    // Mirror of the upload-side feature: when the flag is on the download pre-creates
    // any missing parent directories locally, so a sync into a freshly-diffed subtree
    // that doesn't yet exist on disk doesn't fail at ofstream::open time.

    TEST_F(DownloadOperationTests, DownloadCreatesSingleMissingLocalParentDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "newly_created" / "file1.txt";
        ASSERT_FALSE(std::filesystem::exists(localPath.parent_path()));

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .createMissingDirectories = true,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);
        ASSERT_TRUE(std::filesystem::exists(localPath));
        EXPECT_TRUE(std::filesystem::is_directory(localPath.parent_path()));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, DownloadCreatesDeeplyNestedMissingLocalParentDirectories)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "a" / "b" / "c" / "d" / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .createMissingDirectories = true,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);
        EXPECT_TRUE(std::filesystem::is_directory(downloadDir_.path() / "a"));
        EXPECT_TRUE(std::filesystem::is_directory(downloadDir_.path() / "a" / "b"));
        EXPECT_TRUE(std::filesystem::is_directory(downloadDir_.path() / "a" / "b" / "c"));
        EXPECT_TRUE(std::filesystem::is_directory(downloadDir_.path() / "a" / "b" / "c" / "d"));
        ASSERT_TRUE(std::filesystem::exists(localPath));
    }

    TEST_F(DownloadOperationTests, DownloadWithoutCreateMissingDirectoriesFailsOnMissingParent)
    {
        // Default behaviour: the std::ofstream::open fails and the operation enters error state.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "nonexistent" / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .createMissingDirectories = false,
            }};

        auto first = operation.work();
        // The error can surface on any iteration before completion.
        for (int i = 0; first.has_value() && *first != DownloadOperation::WorkStatus::Complete && i < 100; ++i)
            first = operation.work();

        ASSERT_FALSE(first.has_value()) << "download should fail when local parent is missing";
        EXPECT_FALSE(std::filesystem::exists(localPath.parent_path()))
            << "parent dir must NOT have been created";
    }

    TEST_F(DownloadOperationTests, DownloadWithCreateMissingDirectoriesIsIdempotentOnExistingParent)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/file1.txt",
                .localPath = localPath,
                .createMissingDirectories = true,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(DownloadOperationTests, DownloadSymlinkCreatesMissingLocalParentDirectories)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "nested" / "deep" / "link_to_file1.txt";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/link_to_file1.txt",
                .localPath = localPath,
                .createMissingDirectories = true,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);
        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(localPath)));
    }

    TEST_F(DownloadOperationTests, DownloadDanglingRemoteSymlinkCreatesLocalSymlink)
    {
        // Even when the remote link points nowhere, downloading it must still result in a
        // local symlink containing the same literal target. The UX requirement: syncing a
        // dangling link should not surface as an error.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = downloadDir_.path() / "dangling_link";

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/dangling_link",
                .localPath = localPath,
            }};

        EXPECT_EQ(runDownloadToCompletion(operation), DownloadOperation::WorkStatus::Complete);
        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(localPath)));
    }
}
