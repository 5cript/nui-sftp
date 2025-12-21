#pragma once

#include <backend/sftp/upload_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>

namespace Test
{
    class UploadOperationTests : public RealServerTests
    {
      public:
        std::string testFileContent{"This is a test file for upload operation tests.\n"};

        void SetUp() override
        {
            RealServerTests::SetUp();

            std::ofstream testFile{programDirectory / "temp" / "testfile.txt", std::ios_base::binary};
            testFile << testFileContent;
        }
    };

    TEST_F(UploadOperationTests, CanCreateServer)
    {
        /*self test*/
        CREATE_SERVER_AND_JOINER(sftpServer);
    }

    TEST_F(UploadOperationTests, CanCreateUploadOperation)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{*sftp, {}};
    }

    TEST_F(UploadOperationTests, UploadOperationTypeIsUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        UploadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::Upload);
    }

    TEST_F(UploadOperationTests, CanPrepareUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
            }};
        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());

        auto fut = sftp->listDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto listResult = fut.get();
        ASSERT_TRUE(listResult.has_value());
        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "testfile.txt.filepart";
        });

        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(UploadOperationTests, CanPrepareUploadByAdoptingFilepart)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto createFuture = sftp->createFile("/home/test/testfile.txt.filepart");
        ASSERT_EQ(createFuture.wait_for(5s), std::future_status::ready);
        auto createResult = createFuture.get();
        ASSERT_TRUE(createResult.has_value());

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
                .tryContinue = true,
            }};
        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());

        auto fut = sftp->listDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto listResult = fut.get();
        ASSERT_TRUE(listResult.has_value());
        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "testfile.txt.filepart";
        });

        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(UploadOperationTests, CanUploadFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
            }};

        UploadOperation::WorkStatus workStatus;
        for (int workTimes = 0; workTimes < 100; ++workTimes)
        {
            auto workResult = operation.work();
            ASSERT_TRUE(workResult.has_value());
            workStatus = workResult.value();
            if (workStatus == UploadOperation::WorkStatus::Complete)
                break;
        }
        ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

        auto fut = sftp->listDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto listResult = fut.get();
        ASSERT_TRUE(listResult.has_value());
        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "testfile.txt";
        });
        EXPECT_NE(it, listResult.value().end());

        auto closeResult = operation.cancel(false);
        ASSERT_TRUE(closeResult.has_value());

        auto readFut = sftp->openFile(
            "/home/test/testfile.txt", SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::owner_read);
        ASSERT_EQ(readFut.wait_for(5s), std::future_status::ready);
        auto readResult = readFut.get();

        ASSERT_TRUE(readResult.has_value());
        auto fileWeak = std::move(readResult).value();
        auto file = fileWeak.lock();
        ASSERT_TRUE(file);

        std::string data(testFileContent.size() + 100, '\0');
        auto readAllFut = file->readSome(data.data(), data.size());
        ASSERT_EQ(readAllFut.wait_for(5s), std::future_status::ready);
        auto readAllResult = readAllFut.get();
        ASSERT_TRUE(readAllResult.has_value());
        data.resize(readAllResult.value());

        EXPECT_EQ(data, testFileContent);
    }

    TEST_F(UploadOperationTests, UploadFailsWhenFileExists)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // First upload
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                }};

            UploadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == UploadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

            auto closeResult = operation.cancel(false);
            ASSERT_TRUE(closeResult.has_value());
        }

        // Second upload without overwrite
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                    .mayOverwrite = false,
                }};

            auto workResult = operation.work();
            ASSERT_FALSE(workResult.has_value());
            EXPECT_EQ(workResult.error().type, UploadOperation::ErrorType::FileExists);
        }
    }

    TEST_F(UploadOperationTests, UploadOverwritesExistingFileWhenEnabled)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // First upload
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                }};

            UploadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == UploadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

            auto closeResult = operation.cancel(false);
            ASSERT_TRUE(closeResult.has_value());
        }

        // Second upload with overwrite
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                    .mayOverwrite = true,
                }};

            UploadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == UploadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

            auto closeResult = operation.cancel(false);
            ASSERT_TRUE(closeResult.has_value());
        }
    }

    TEST_F(UploadOperationTests, CanContinuePartialFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // First upload, but only part of it
        {
            auto openFut = sftp->openFile(
                "/home/test/testfile.txt.filepart",
                SecureShell::SftpSession::OpenType::Write | SecureShell::SftpSession::OpenType::Create,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
            ASSERT_EQ(openFut.wait_for(5s), std::future_status::ready);
            auto openResult = openFut.get();
            ASSERT_TRUE(openResult.has_value());
            auto fileWeak = std::move(openResult).value();
            auto file = fileWeak.lock();
            ASSERT_TRUE(file);

            std::string partialContent = testFileContent.substr(0, testFileContent.size() / 2);
            auto writeFut = file->write(partialContent);
            ASSERT_EQ(writeFut.wait_for(5s), std::future_status::ready);
            writeFut.get();
            file->close();
        }

        // Second upload continuing the partial file
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                    .tryContinue = true,
                }};

            UploadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == UploadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

            auto closeResult = operation.cancel(false);
            ASSERT_TRUE(closeResult.has_value());
        }

        // Verify final file content
        {
            auto readFut = sftp->openFile(
                "/home/test/testfile.txt",
                SecureShell::SftpSession::OpenType::Read,
                std::filesystem::perms::owner_read);
            ASSERT_EQ(readFut.wait_for(5s), std::future_status::ready);
            auto readResult = readFut.get();

            ASSERT_TRUE(readResult.has_value());
            auto fileWeak = std::move(readResult).value();
            auto file = fileWeak.lock();
            ASSERT_TRUE(file);

            std::string data(testFileContent.size() + 100, '\0');
            auto readAllFut = file->readSome(data.data(), data.size());
            ASSERT_EQ(readAllFut.wait_for(5s), std::future_status::ready);
            auto readAllResult = readAllFut.get();
            ASSERT_TRUE(readAllResult.has_value());
            data.resize(readAllResult.value());

            EXPECT_EQ(data, testFileContent);
        }
    }

    TEST_F(UploadOperationTests, PartialFileLargerThanLocalDoesNotContinueButWorksAnyways)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // First upload, but only part of it
        {
            auto openFut = sftp->openFile(
                "/home/test/testfile.txt.filepart",
                SecureShell::SftpSession::OpenType::Write | SecureShell::SftpSession::OpenType::Create,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
            ASSERT_EQ(openFut.wait_for(5s), std::future_status::ready);
            auto openResult = openFut.get();
            ASSERT_TRUE(openResult.has_value());
            auto fileWeak = std::move(openResult).value();
            auto file = fileWeak.lock();
            ASSERT_TRUE(file);

            std::string partialContent = testFileContent + "Extra data to make the partial file larger.";
            auto writeFut = file->write(partialContent);
            ASSERT_EQ(writeFut.wait_for(5s), std::future_status::ready);
            writeFut.get();
            file->close();
        }

        // Second upload attempting to continue the partial file
        {
            UploadOperation operation{
                *sftp,
                {
                    .remotePath = "/home/test/testfile.txt",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                    .tryContinue = true,
                }};

            UploadOperation::WorkStatus workStatus;
            for (int workTimes = 0; workTimes < 100; ++workTimes)
            {
                auto workResult = operation.work();
                ASSERT_TRUE(workResult.has_value());
                workStatus = workResult.value();
                if (workStatus == UploadOperation::WorkStatus::Complete)
                    break;
            }
            ASSERT_EQ(workStatus, UploadOperation::WorkStatus::Complete);

            auto closeResult = operation.cancel(false);
            ASSERT_TRUE(closeResult.has_value());
        }
    }
}