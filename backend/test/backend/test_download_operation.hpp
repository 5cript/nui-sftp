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
}
