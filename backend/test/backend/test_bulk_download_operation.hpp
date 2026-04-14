#pragma once

#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>
#include <utility/temporary_directory.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Test
{
    class BulkDownloadOperationTests : public RealServerTests
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
        struct ScanBundle
        {
            std::vector<SharedData::DirectoryEntry> entries;
            std::uint64_t totalBytes;
        };

        ScanBundle
        runScan(SecureShell::SftpSession& sftp, std::filesystem::path const& remotePath, bool recursive = true)
        {
            ScanOperation scan{sftp, {.remotePath = remotePath, .recursive = recursive}};
            for (int iteration = 0; iteration < 500; ++iteration)
            {
                auto workResult = scan.work();
                EXPECT_TRUE(workResult.has_value()) << "ScanOperation::work() failed during test setup";
                if (!workResult.has_value())
                    break;
                if (workResult.value() == ScanOperation::WorkStatus::Complete)
                    break;
            }
            const auto bytes = scan.totalBytes();
            return ScanBundle{.entries = scan.ejectEntries(), .totalBytes = bytes};
        }

        static BulkDownloadOperation::WorkStatus
        runBulkToCompletion(BulkDownloadOperation& operation, int maxIterations = 2000)
        {
            BulkDownloadOperation::WorkStatus last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                auto workResult = operation.work();
                EXPECT_TRUE(workResult.has_value()) << "work() failed unexpectedly";
                if (!workResult.has_value())
                    return last;
                last = workResult.value();
                if (last == BulkDownloadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

      protected:
        Utility::TemporaryDirectory downloadDir_{programDirectory / "temp", true};
    };

    TEST_F(BulkDownloadOperationTests, CanCreateBulkDownloadOperation)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkDownloadOperation operation{*sftp, {}};
    }

    TEST_F(BulkDownloadOperationTests, BulkDownloadOperationTypeIsBulkDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkDownloadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::BulkDownload);
    }

    TEST_F(BulkDownloadOperationTests, BulkDownloadOperationIsNotBarrier)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        BulkDownloadOperation operation{*sftp, {}};
        EXPECT_FALSE(operation.isBarrier());
    }

    TEST_F(BulkDownloadOperationTests, EmptyEntriesCompletesImmediately)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{*sftp, {.localPath = downloadDir_.path() / "empty"}};
        operation.setScanResult({}, 0);

        auto workResult = operation.work();
        ASSERT_TRUE(workResult.has_value());
        EXPECT_EQ(workResult.value(), BulkDownloadOperation::WorkStatus::Complete);
    }

    TEST_F(BulkDownloadOperationTests, FirstEntryMustBeDirectory)
    {
        // A bulk download is rooted at a directory entry. If somebody feeds in scan results whose
        // first entry is a regular file, the operation must produce a clean error rather than
        // proceed into undefined territory.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{*sftp, {.localPath = downloadDir_.path() / "bad"}};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test/file1.txt",
            .type = SharedData::FileType::Regular,
            .size = 17,
        });
        operation.setScanResult(std::move(entries), 17);

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, BulkDownloadOperation::ErrorType::ImplementationError);
    }

    TEST_F(BulkDownloadOperationTests, DownloadsFlatDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");
        ASSERT_FALSE(scan.entries.empty());

        const auto localRoot = downloadDir_.path() / "Documents";

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_directory(localRoot));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc2.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "doc1.txt"), "Document 1 content");
        EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkDownloadOperationTests, CreatesRootLocalDirectoryIfMissing)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        const auto localRoot = downloadDir_.path() / "newly" / "created" / "root";
        ASSERT_FALSE(std::filesystem::exists(localRoot));

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_TRUE(std::filesystem::is_directory(localRoot));
        EXPECT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
    }

    TEST_F(BulkDownloadOperationTests, CreatesNestedDirectoriesForDescendingTree)
    {
        // Fabricated entries mimic a scan with a nested sub-directory, since the fixture
        // filesystem is only one level deep per subtree.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "nested";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test",
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "Documents",
            .type = SharedData::FileType::Directory,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "doc1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Document 1 content"}.size(),
            .parent = 1,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setScanResult(std::move(entries), std::string{"Document 1 content"}.size());

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_directory(localRoot / "Documents"));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "Documents" / "doc1.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "Documents" / "doc1.txt"), "Document 1 content");
    }

    TEST_F(BulkDownloadOperationTests, FailingEntryIsCollectedAndRestContinue)
    {
        // When a file's download fails with a recoverable sftp error, bulk download must skip it,
        // record it in getFailed(), and keep going on the remaining entries.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "partial";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test",
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "does_not_exist.txt",
            .type = SharedData::FileType::Regular,
            .size = 10,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Fake file content"}.size(),
            .parent = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot, .failFast = false}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::exists(localRoot / "file1.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        EXPECT_FALSE(std::filesystem::exists(localRoot / "does_not_exist.txt"));

        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_EQ(failed.front().first.filename().generic_string(), "does_not_exist.txt");
    }

    TEST_F(BulkDownloadOperationTests, FailFastAbortsWholeOperationOnRecoverableError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "failfast";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test",
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "does_not_exist.txt",
            .type = SharedData::FileType::Regular,
            .size = 10,
            .parent = 0,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "file1.txt",
            .type = SharedData::FileType::Regular,
            .size = std::string{"Fake file content"}.size(),
            .parent = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot, .failFast = true}};
        operation.setScanResult(std::move(entries), 0);

        std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> workResult{
            BulkDownloadOperation::WorkStatus::MoreWork};
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            workResult = operation.work();
            if (!workResult.has_value())
                break;
            if (workResult.value() == BulkDownloadOperation::WorkStatus::Complete)
                break;
        }

        ASSERT_FALSE(workResult.has_value()) << "failFast must propagate the error out of work()";
        // The later entry must not have been downloaded.
        EXPECT_FALSE(std::filesystem::exists(localRoot / "file1.txt"));
    }

    TEST_F(BulkDownloadOperationTests, UnsupportedEntryTypesAreSkipped)
    {
        // Special/Unknown entries aren't uploadable and must be skipped without failing.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "skip_unsupported";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test",
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
            .size = std::string{"Fake file content"}.size(),
            .parent = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_TRUE(std::filesystem::exists(localRoot / "file1.txt"));
        EXPECT_FALSE(std::filesystem::exists(localRoot / "weird"));
    }

    TEST_F(BulkDownloadOperationTests, SymlinkEntriesAreRecreatedAsSymlinks)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "with_symlinks";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test",
            .type = SharedData::FileType::Directory,
        });
        entries.push_back(SharedData::DirectoryEntry{
            .path = "link_to_file1.txt",
            .type = SharedData::FileType::Symlink,
            .parent = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        const auto linkPath = localRoot / "link_to_file1.txt";
        EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(linkPath)));
    }

    TEST_F(BulkDownloadOperationTests, ProgressCallbackInvokedDuringBulkDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        int callbackInvocations = 0;
        BulkDownloadOperation operation{
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
                .localPath = downloadDir_.path() / "progress",
            }};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(BulkDownloadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        BulkDownloadOperation operation{*sftp, {.localPath = downloadDir_.path() / "completed"}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        auto secondCallResult = operation.work();
        ASSERT_FALSE(secondCallResult.has_value());
        EXPECT_EQ(secondCallResult.error().type, BulkDownloadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(BulkDownloadOperationTests, WorkOnFailedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{*sftp, {.localPath = downloadDir_.path() / "failed"}};

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{
            .path = "/home/test/file1.txt",
            .type = SharedData::FileType::Regular,
        });
        operation.setScanResult(std::move(entries), 0);

        auto firstResult = operation.work();
        ASSERT_FALSE(firstResult.has_value());

        auto secondResult = operation.work();
        ASSERT_FALSE(secondResult.has_value());
        EXPECT_EQ(secondResult.error().type, BulkDownloadOperation::ErrorType::CannotWorkFailedOperation);
    }

    TEST_F(BulkDownloadOperationTests, SetPrescannedFileListDownloadsAllFilesInSingleOperation)
    {
        // The bulk optimization: a single BulkDownloadOperation primed via
        // setPrescannedFileList should transfer N pre-known files end-to-end
        // without spawning per-file scan passes.  Run to completion and verify
        // every file lands locally with its original content.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "prescanned";
        std::filesystem::create_directories(localRoot);

        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .remoteSrc = "/home/test/Documents/doc1.txt",
            .localDst = localRoot / "doc1.txt",
            .sizeBytes = std::string{"Document 1 content"}.size(),
        });
        files.push_back({
            .remoteSrc = "/home/test/Documents/doc2.txt",
            .localDst = localRoot / "doc2.txt",
            .sizeBytes = std::string{"Document 2 content"}.size(),
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc2.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "doc1.txt"), "Document 1 content");
        EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkDownloadOperationTests, SetPrescannedFileListPreservesMtimeOnDownloadedFiles)
    {
        // Regression: BulkAddEntry → PrescannedFile → DirectoryEntry must
        // carry mtime through so each downloaded local file ends up with the
        // remote's mtime (otherwise a follow-up sync would see all downloaded
        // files as "modified" and re-enqueue them forever).
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "prescanned_mtime";
        std::filesystem::create_directories(localRoot);

        constexpr std::uint64_t mtimeDoc1 = 1234567890ull; // 2009-02-13
        constexpr std::uint64_t mtimeDoc2 = 1600000000ull; // 2020-09-13

        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .remoteSrc = "/home/test/Documents/doc1.txt",
            .localDst = localRoot / "doc1.txt",
            .sizeBytes = std::string{"Document 1 content"}.size(),
            .mtime = mtimeDoc1,
            .mtimeNsec = 0,
        });
        files.push_back({
            .remoteSrc = "/home/test/Documents/doc2.txt",
            .localDst = localRoot / "doc2.txt",
            .sizeBytes = std::string{"Document 2 content"}.size(),
            .mtime = mtimeDoc2,
            .mtimeNsec = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = localRoot}};
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        const auto mtimeSeconds = [](std::filesystem::path const& path) {
            const auto fileTime = std::filesystem::last_write_time(path);
            const auto sysTime = std::chrono::file_clock::to_sys(fileTime);
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch()).count()
            );
        };

        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc2.txt"));
        EXPECT_EQ(mtimeSeconds(localRoot / "doc1.txt"), mtimeDoc1);
        EXPECT_EQ(mtimeSeconds(localRoot / "doc2.txt"), mtimeDoc2);
    }

    TEST_F(BulkDownloadOperationTests, CancelledOperationCannotBeWorkedFurther)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        BulkDownloadOperation operation{*sftp, {.localPath = downloadDir_.path() / "cancel"}};
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, BulkDownloadOperation::ErrorType::CannotWorkCanceledOperation);
    }
}
