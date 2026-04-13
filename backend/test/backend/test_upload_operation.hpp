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

    TEST_F(UploadOperationTests, EmptyFileUploadSucceeds)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::ofstream emptyFile{programDirectory / "temp" / "emptyfile.txt", std::ios_base::binary};
        emptyFile.close();

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/emptyfile.txt",
                .localPath = programDirectory / "temp" / "emptyfile.txt",
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
            return entry.path == "emptyfile.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    TEST_F(UploadOperationTests, FilepartAbsentAfterSuccessfulUpload)
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
        auto filepartIt =
            std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
                return entry.path == "testfile.txt.filepart";
            });
        EXPECT_EQ(filepartIt, listResult.value().end());
    }

    TEST_F(UploadOperationTests, WorkOnCompletedOperationReturnsError)
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

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, UploadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(UploadOperationTests, WorkOnFailedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "does_not_exist.txt",
            }};

        auto firstResult = operation.work();
        ASSERT_FALSE(firstResult.has_value());

        auto secondResult = operation.work();
        ASSERT_FALSE(secondResult.has_value());
        EXPECT_EQ(secondResult.error().type, UploadOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(UploadOperationTests, UploadToNonExistentRemoteDirectoryFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/nonexistent_dir/file.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, UploadOperation::ErrorType::SftpError);
    }

    TEST_F(UploadOperationTests, UploadNonExistentLocalFileFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "does_not_exist.txt",
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, UploadOperation::ErrorType::NotAFile);
    }

    TEST_F(UploadOperationTests, UploadLocalDirectoryFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory,
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, UploadOperation::ErrorType::NotAFile);
    }

    TEST_F(UploadOperationTests, LargeFileUploadSucceeds)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // File larger than the internal buffer (16384 bytes) to exercise multi-chunk upload
        const std::string largeContent(30000, 'x');
        {
            std::ofstream largeFile{programDirectory / "temp" / "largefile.txt", std::ios_base::binary};
            largeFile << largeContent;
        }

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/largefile.txt",
                .localPath = programDirectory / "temp" / "largefile.txt",
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
            return entry.path == "largefile.txt";
        });
        EXPECT_NE(it, listResult.value().end());
    }

    // ---- Symlink handling --------------------------------------------------
    //
    // These tests exercise the paths that caused issues during sync development:
    //   - symlinks must be recreated as links on the remote, not have their target's
    //     content uploaded;
    //   - this applies equally to links whose target is a file, a directory, or doesn't
    //     exist at all (dangling);
    //   - re-running sync against a previously-synced tree must overwrite an existing
    //     remote entry rather than error out.

    namespace
    {
        // Drive UploadOperation::work() to completion or failure. Returns the final status.
        inline std::expected<UploadOperation::WorkStatus, UploadOperation::Error>
        runToCompletion(UploadOperation& op, int maxIterations = 100)
        {
            UploadOperation::WorkStatus last{};
            for (int i = 0; i < maxIterations; ++i)
            {
                auto result = op.work();
                if (!result.has_value())
                    return std::unexpected(result.error());
                last = result.value();
                if (last == UploadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        inline std::filesystem::path makeFreshSymlink(
            std::filesystem::path const& linkPath,
            std::filesystem::path const& target
        )
        {
            std::error_code ec;
            std::filesystem::remove(linkPath, ec);
            std::filesystem::create_symlink(target, linkPath);
            return linkPath;
        }
    }

    TEST_F(UploadOperationTests, UploadLocalSymlinkToRegularFileCreatesRemoteSymlink)
    {
        // Set up: a local symlink to a regular file.
        const auto targetFile = programDirectory / "temp" / "sym_target_file.txt";
        {
            std::ofstream f{targetFile, std::ios::binary};
            f << "target content";
        }
        const auto linkPath = makeFreshSymlink(programDirectory / "temp" / "sym_to_file", targetFile);

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/uploaded_sym_to_file",
                .localPath = linkPath,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        // The remote entry must be a symlink, not a file copy of the target.
        auto lstatFut = sftp->lstat("/home/test/uploaded_sym_to_file");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink())
            << "remote entry should be a symlink, was type=" << static_cast<int>(entry->type);
    }

    TEST_F(UploadOperationTests, UploadLocalSymlinkToDirectoryCreatesRemoteSymlinkNotRecursiveCopy)
    {
        // A symlink pointing at a directory must be recreated as a single remote link;
        // the directory contents must NOT be recursively uploaded into it.
        const auto targetDir = programDirectory / "temp" / "sym_target_dir";
        std::error_code ec;
        std::filesystem::create_directory(targetDir, ec);
        {
            std::ofstream f{targetDir / "inside.txt", std::ios::binary};
            f << "should not appear on remote";
        }
        const auto linkPath = makeFreshSymlink(programDirectory / "temp" / "sym_to_dir", targetDir);

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/uploaded_sym_to_dir",
                .localPath = linkPath,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/uploaded_sym_to_dir");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink());
    }

    TEST_F(UploadOperationTests, UploadDanglingLocalSymlinkSucceeds)
    {
        // A dangling symlink must upload cleanly (as a link to the same literal target),
        // without trying to open/stat the target. This is the scenario that caused the
        // "Path exists and is not a directory" and FileStatFailed errors before.
        const auto linkPath = makeFreshSymlink(
            programDirectory / "temp" / "dangling_upload",
            "/nowhere/this/should/not/exist.txt"
        );

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/uploaded_dangling",
                .localPath = linkPath,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value()) << "dangling symlink upload failed";
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/uploaded_dangling");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink());
    }

    TEST_F(UploadOperationTests, UploadSymlinkWithSkipSymlinkDoesNotCreateRemoteEntry)
    {
        const auto targetFile = programDirectory / "temp" / "sym_skip_target.txt";
        {
            std::ofstream f{targetFile, std::ios::binary};
            f << "content";
        }
        const auto linkPath = makeFreshSymlink(programDirectory / "temp" / "sym_skip", targetFile);

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/uploaded_sym_skip",
                .localPath = linkPath,
                .symlinkHandling = Persistence::SymlinkHandling::SkipSymlink,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/uploaded_sym_skip");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        EXPECT_FALSE(entry.has_value()) << "SkipSymlink must not create a remote entry";
    }

    TEST_F(UploadOperationTests, UploadSymlinkOverwritesExistingRemoteFile)
    {
        // Scenario: sync was run before, producing a regular file at this path; the user now
        // syncs a symlink at the same path. With mayOverwrite=true the remote file must be
        // replaced by a symlink.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // First put a regular file in place.
        {
            UploadOperation first{
                *sftp,
                {
                    .remotePath = "/home/test/sym_overwrite_target",
                    .localPath = programDirectory / "temp" / "testfile.txt",
                }};
            auto s = runToCompletion(first);
            ASSERT_TRUE(s.has_value());
        }

        // Now overwrite with a symlink.
        const auto linkPath = makeFreshSymlink(
            programDirectory / "temp" / "sym_overwrite_link",
            programDirectory / "temp" / "testfile.txt"
        );
        UploadOperation symOp{
            *sftp,
            {
                .remotePath = "/home/test/sym_overwrite_target",
                .localPath = linkPath,
                .mayOverwrite = true,
            }};
        auto status = runToCompletion(symOp);
        ASSERT_TRUE(status.has_value()) << "symlink-over-file overwrite failed";
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/sym_overwrite_target");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink())
            << "expected remote entry to be replaced with a symlink";
    }

    TEST_F(UploadOperationTests, UploadSymlinkPrepareDoesNotOpenTempFile)
    {
        // For symlinks, prepare() must not open a .filepart file on the remote — that path
        // is only for regular-file uploads.
        const auto linkPath = makeFreshSymlink(
            programDirectory / "temp" / "sym_prepare_check",
            programDirectory / "temp" / "testfile.txt"
        );

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/sym_prepare_check",
                .localPath = linkPath,
            }};

        auto prepareResult = operation.prepare();
        ASSERT_TRUE(prepareResult.has_value());

        auto fut = sftp->listDirectory("/home/test");
        ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
        auto listResult = fut.get();
        ASSERT_TRUE(listResult.has_value());
        auto it = std::find_if(listResult.value().begin(), listResult.value().end(), [](const auto& entry) {
            return entry.path == "sym_prepare_check.filepart";
        });
        EXPECT_EQ(it, listResult.value().end()) << ".filepart must not be created for symlink uploads";
    }

    // ---- createMissingDirectories option ----------------------------------
    //
    // These exercise the sync-initiated path where the remote subtree may not yet
    // exist.  Without the flag an upload into /home/test/missing/... fails (see
    // UploadToNonExistentRemoteDirectoryFails above); with the flag the operation
    // walks up the chain and mkdirs every missing level.

    TEST_F(UploadOperationTests, UploadCreatesSingleMissingParentDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/newly_created/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
                .createMissingDirectories = true,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value()) << "upload should succeed when parent is auto-created";
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/newly_created/testfile.txt");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        ASSERT_TRUE(lstatFut.get().has_value()) << "uploaded file should exist at the nested path";

        auto dirFut = sftp->lstat("/home/test/newly_created");
        ASSERT_EQ(dirFut.wait_for(5s), std::future_status::ready);
        auto dirEntry = dirFut.get();
        ASSERT_TRUE(dirEntry.has_value());
        EXPECT_EQ(dirEntry->type, SharedData::FileType::Directory);
    }

    TEST_F(UploadOperationTests, UploadCreatesDeeplyNestedMissingParentDirectories)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/a/b/c/d/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
                .createMissingDirectories = true,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value()) << "deeply nested upload should succeed";
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        for (auto const* path : {"/home/test/a", "/home/test/a/b", "/home/test/a/b/c", "/home/test/a/b/c/d"})
        {
            auto fut = sftp->lstat(path);
            ASSERT_EQ(fut.wait_for(5s), std::future_status::ready);
            auto entry = fut.get();
            ASSERT_TRUE(entry.has_value()) << "intermediate directory missing: " << path;
            EXPECT_EQ(entry->type, SharedData::FileType::Directory) << path;
        }

        auto fileFut = sftp->lstat("/home/test/a/b/c/d/testfile.txt");
        ASSERT_EQ(fileFut.wait_for(5s), std::future_status::ready);
        EXPECT_TRUE(fileFut.get().has_value());
    }

    TEST_F(UploadOperationTests, UploadWithCreateMissingDirectoriesIsIdempotentOnExistingParent)
    {
        // When the parent already exists the flag must be a no-op (no mkdir error).
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
                .createMissingDirectories = true,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);
    }

    TEST_F(UploadOperationTests, UploadWithoutCreateMissingDirectoriesStillFailsOnMissingParent)
    {
        // Regression guard: default behaviour must not silently start creating dirs.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/still_missing_dir/testfile.txt",
                .localPath = programDirectory / "temp" / "testfile.txt",
                .createMissingDirectories = false,
            }};

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, UploadOperation::ErrorType::SftpError);

        auto dirFut = sftp->lstat("/home/test/still_missing_dir");
        ASSERT_EQ(dirFut.wait_for(5s), std::future_status::ready);
        EXPECT_FALSE(dirFut.get().has_value()) << "parent dir must NOT have been created";
    }

    TEST_F(UploadOperationTests, UploadSymlinkCreatesMissingParentDirectories)
    {
        // Symlink path must honour the flag too: createSymLink fails without the parent.
        const auto linkPath = makeFreshSymlink(
            programDirectory / "temp" / "sym_for_missing_dirs",
            programDirectory / "temp" / "testfile.txt"
        );

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = "/home/test/missing/for/symlink/uploaded_link",
                .localPath = linkPath,
                .createMissingDirectories = true,
            }};

        auto status = runToCompletion(operation);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, UploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat("/home/test/missing/for/symlink/uploaded_link");
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink());
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