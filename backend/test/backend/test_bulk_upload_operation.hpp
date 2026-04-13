#pragma once

#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>
#include <utility/temporary_directory.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace Test
{
    class BulkUploadOperationTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            RealServerTests::SetUp();

            writeFile(uploadSrc_.path() / "file1.txt", "Bulk upload file one");
            writeFile(uploadSrc_.path() / "file2.txt", "Bulk upload file two");
        }

      protected:
        static void writeFile(std::filesystem::path const& path, std::string_view contents)
        {
            std::ofstream stream{path, std::ios::binary};
            stream << contents;
        }

        struct ScanBundle
        {
            std::vector<SharedData::DirectoryEntry> entries;
            std::uint64_t totalBytes;
        };

        ScanBundle runLocalScan(std::filesystem::path const& localPath, bool recursive = true)
        {
            LocalScanOperation scan{{.localPath = localPath, .recursive = recursive}};
            for (int iteration = 0; iteration < 500; ++iteration)
            {
                auto workResult = scan.work();
                EXPECT_TRUE(workResult.has_value()) << "LocalScanOperation::work() failed during test setup";
                if (!workResult.has_value())
                    break;
                if (workResult.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
            const auto bytes = scan.totalBytes();
            return ScanBundle{.entries = scan.ejectEntries(), .totalBytes = bytes};
        }

        static BulkUploadOperation::WorkStatus
        runBulkToCompletion(BulkUploadOperation& operation, int maxIterations = 2000)
        {
            BulkUploadOperation::WorkStatus last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                auto workResult = operation.work();
                EXPECT_TRUE(workResult.has_value()) << "work() failed unexpectedly";
                if (!workResult.has_value())
                    return last;
                last = workResult.value();
                if (last == BulkUploadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        static bool remoteHasEntry(SecureShell::SftpSession& sftp, std::filesystem::path const& remoteDir,
                                   std::string const& filename)
        {
            auto fut = sftp.listDirectory(remoteDir);
            if (fut.wait_for(5s) != std::future_status::ready)
                return false;
            auto result = fut.get();
            if (!result.has_value())
                return false;
            return std::any_of(result.value().begin(), result.value().end(), [&](auto const& entry) {
                return entry.path.filename().generic_string() == filename;
            });
        }

      protected:
        Utility::TemporaryDirectory uploadSrc_{programDirectory / "temp", true};
    };

    TEST_F(BulkUploadOperationTests, CanCreateBulkUploadOperation)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkUploadOperation operation{*sftp, {}};
    }

    TEST_F(BulkUploadOperationTests, BulkUploadOperationTypeIsBulkUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkUploadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::BulkUpload);
    }

    TEST_F(BulkUploadOperationTests, BulkUploadOperationIsNotBarrier)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkUploadOperation operation{*sftp, {}};
        EXPECT_FALSE(operation.isBarrier());
    }

    TEST_F(BulkUploadOperationTests, EmptyEntriesCompletesImmediately)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkUploadOperation operation{*sftp, {.remotePath = "/home/test/empty_bulk", .localPath = uploadSrc_.path()}};
        operation.setScanResult({}, 0);

        auto workResult = operation.work();
        ASSERT_TRUE(workResult.has_value());
        EXPECT_EQ(workResult.value(), BulkUploadOperation::WorkStatus::Complete);
    }

    TEST_F(BulkUploadOperationTests, FirstEntryMustBeDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = "/home/test/bad_bulk", .localPath = uploadSrc_.path()}};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path() / "file1.txt",
            .type = SharedData::FileType::Regular,
        });
        operation.setScanResult(std::move(entries), 0);

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, BulkUploadOperation::ErrorType::ImplementationError);
    }

    TEST_F(BulkUploadOperationTests, UploadsFlatDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(uploadSrc_.path());
        ASSERT_FALSE(scan.entries.empty());

        const auto remoteRoot = std::filesystem::path{"/home/test/bulk_uploaded"};

        BulkUploadOperation operation{*sftp, {.remotePath = remoteRoot, .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);

        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot, "file1.txt"));
        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot, "file2.txt"));
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkUploadOperationTests, CreatesNestedRemoteDirectories)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto remoteRoot = std::filesystem::path{"/home/test/nested_bulk"};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path(),
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "subdir",
            .type = SharedData::FileType::Directory,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Bulk upload file one"}.size(),
            .parent = 1,
        });

        // Required local file at <localPath>/subdir/file1.txt
        std::filesystem::create_directory(uploadSrc_.path() / "subdir");
        writeFile(uploadSrc_.path() / "subdir" / "file1.txt", "Bulk upload file one");

        BulkUploadOperation operation{*sftp, {.remotePath = remoteRoot, .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(entries), std::string{"Bulk upload file one"}.size());

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);

        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot, "subdir"));
        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot / "subdir", "file1.txt"));
    }

    TEST_F(BulkUploadOperationTests, FailingLocalFileIsCollectedAndRestContinue)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto remoteRoot = std::filesystem::path{"/home/test/partial_bulk"};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path(),
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "does_not_exist_locally.txt",
            .type = SharedData::FileType::Regular,
            .size = 10,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Bulk upload file one"}.size(),
            .parent = 0,
        });

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = remoteRoot, .localPath = uploadSrc_.path(), .failFast = false}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);

        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot, "file1.txt"));
        EXPECT_FALSE(remoteHasEntry(*sftp, remoteRoot, "does_not_exist_locally.txt"));

        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_EQ(failed.front().first.filename().generic_string(), "does_not_exist_locally.txt");
    }

    TEST_F(BulkUploadOperationTests, FailFastAbortsWholeOperationOnRecoverableError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto remoteRoot = std::filesystem::path{"/home/test/failfast_bulk"};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path(),
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "does_not_exist_locally.txt",
            .type = SharedData::FileType::Regular,
            .size = 10,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Bulk upload file one"}.size(),
            .parent = 0,
        });

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = remoteRoot, .localPath = uploadSrc_.path(), .failFast = true}};
        operation.setScanResult(std::move(entries), 0);

        std::expected<BulkUploadOperation::WorkStatus, BulkUploadOperation::Error> workResult{
            BulkUploadOperation::WorkStatus::MoreWork};
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            workResult = operation.work();
            if (!workResult.has_value())
                break;
            if (workResult.value() == BulkUploadOperation::WorkStatus::Complete)
                break;
        }

        ASSERT_FALSE(workResult.has_value()) << "failFast must propagate the error out of work()";
        EXPECT_FALSE(remoteHasEntry(*sftp, remoteRoot, "file1.txt"));
    }

    TEST_F(BulkUploadOperationTests, UnsupportedEntryTypesAreSkipped)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto remoteRoot = std::filesystem::path{"/home/test/skip_unsupported_bulk"};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path(),
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "weird",
            .type = SharedData::FileType::Fifo,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Bulk upload file one"}.size(),
            .parent = 0,
        });

        BulkUploadOperation operation{*sftp, {.remotePath = remoteRoot, .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);
        EXPECT_TRUE(remoteHasEntry(*sftp, remoteRoot, "file1.txt"));
        EXPECT_FALSE(remoteHasEntry(*sftp, remoteRoot, "weird"));
    }

    TEST_F(BulkUploadOperationTests, SymlinkEntriesAreUploadedAsRemoteSymlinks)
    {
        // Build a local symlink and confirm bulk upload recreates it as a remote symlink
        // rather than uploading the target's bytes.
        const auto targetFile = uploadSrc_.path() / "sym_target.txt";
        writeFile(targetFile, "irrelevant payload");
        const auto linkPath = uploadSrc_.path() / "the_link";
        std::error_code ec;
        std::filesystem::remove(linkPath, ec);
        std::filesystem::create_symlink(targetFile, linkPath);

        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto remoteRoot = std::filesystem::path{"/home/test/symlink_bulk"};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path(),
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "the_link",
            .type = SharedData::FileType::Symlink,
            .linkTarget = targetFile,
            .parent = 0,
        });

        BulkUploadOperation operation{*sftp, {.remotePath = remoteRoot, .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);

        auto lstatFut = sftp->lstat((remoteRoot / "the_link").generic_string());
        ASSERT_EQ(lstatFut.wait_for(5s), std::future_status::ready);
        auto entry = lstatFut.get();
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(entry->isSymlink());
    }

    TEST_F(BulkUploadOperationTests, ProgressCallbackInvokedDuringBulkUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(uploadSrc_.path());

        int callbackInvocations = 0;
        BulkUploadOperation operation{
            *sftp,
            {
                .overallProgressCallback =
                    [&callbackInvocations](
                        std::filesystem::path const&,
                        std::uint64_t,
                        std::uint64_t,
                        std::uint64_t,
                        std::uint64_t,
                        std::uint64_t,
                        std::uint64_t,
                        std::make_signed_t<std::size_t>) { ++callbackInvocations; },
                .remotePath = "/home/test/progress_bulk",
                .localPath = uploadSrc_.path(),
            }};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(BulkUploadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(uploadSrc_.path());

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = "/home/test/completed_bulk", .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkUploadOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, BulkUploadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(BulkUploadOperationTests, WorkOnFailedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = "/home/test/failed_bulk", .localPath = uploadSrc_.path()}};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = uploadSrc_.path() / "file1.txt",
            .type = SharedData::FileType::Regular,
        });
        operation.setScanResult(std::move(entries), 0);

        auto firstResult = operation.work();
        ASSERT_FALSE(firstResult.has_value());

        auto secondResult = operation.work();
        ASSERT_FALSE(secondResult.has_value());
        EXPECT_EQ(secondResult.error().type, BulkUploadOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(BulkUploadOperationTests, CancelledOperationCannotBeWorkedFurther)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(uploadSrc_.path());

        BulkUploadOperation operation{*sftp,
                                      {.remotePath = "/home/test/cancel_bulk", .localPath = uploadSrc_.path()}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, BulkUploadOperation::ErrorType::CannotWorkCanceledOperation);
    }
}
