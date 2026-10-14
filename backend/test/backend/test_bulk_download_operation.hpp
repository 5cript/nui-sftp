#pragma once

#include <backend/sftp/bulk_download_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>
#include <utility/permissions.hpp>
#include <utility/temporary_directory.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

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
            ScanOperation scan{sftp, {.remotePath = remote(remotePath.generic_string()), .recursive = recursive}};
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
        Utility::TemporaryDirectory downloadDir_{temporaryRoot, true};
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

    TEST_F(BulkDownloadOperationTests, EmptyScanResultFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{*sftp, {.localPath = toLocal(downloadDir_.path() / "empty")}};
        // A scanned tree always holds its root, an empty one means the scan never delivered.
        operation.setScanResult({}, 0);

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, SharedData::OperationErrorType::ScanIncomplete);
    }

    TEST_F(BulkDownloadOperationTests, StartWithoutEntriesFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{*sftp, {}};
        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, SharedData::OperationErrorType::ImplementationError);
    }

    TEST_F(BulkDownloadOperationTests, FirstEntryMustBeDirectory)
    {
        // A bulk download is rooted at a directory entry. If somebody feeds in scan results whose
        // first entry is a regular file, the operation must produce a clean error rather than
        // proceed into undefined territory.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test/file1.txt"), .localPath = toLocal(downloadDir_.path() / "bad")}
        };

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "/home/test/file1.txt",
                .type = SharedData::FileType::Regular,
                .size = 17,
            }
        );
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

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test/Documents"), .localPath = toLocal(localRoot)}
        };
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

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test/Documents"), .localPath = toLocal(localRoot)}
        };
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
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "Documents",
                .type = SharedData::FileType::Directory,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "doc1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Document 1 content"}.size(),
                .parent = 1,
            }
        );

        BulkDownloadOperation operation{*sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot)}};
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
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "does_not_exist.txt",
                .type = SharedData::FileType::Regular,
                .size = 10,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot), .failFast = false}
        };
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::exists(localRoot / "file1.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        EXPECT_FALSE(std::filesystem::exists(localRoot / "does_not_exist.txt"));

        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_TRUE(failed.front().first.ends_with("/does_not_exist.txt")) << failed.front().first;
    }

    TEST_F(BulkDownloadOperationTests, ScannedTreeWithoutRootsFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "rootless";
        BulkDownloadOperation operation{*sftp, {.localPath = toLocal(localRoot)}};
        auto scan = runScan(*sftp, "/home/test/Documents");
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        auto workResult = operation.work();
        ASSERT_FALSE(workResult.has_value());
        EXPECT_EQ(workResult.error().type, BulkDownloadOperation::ErrorType::ImplementationError);
        EXPECT_FALSE(std::filesystem::exists(localRoot));
    }

    TEST_F(BulkDownloadOperationTests, NamesThatAreNoPathSegmentAreRecordedAsFailed)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "hostile" / "root";
        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{.type = SharedData::FileType::Directory});
        entries.push_back(
            SharedData::DirectoryEntry{.name = "x/..", .type = SharedData::FileType::Regular, .size = 3, .parent = 0}
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{*sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot)}};
        operation.setScanResult(std::move(entries), 0);
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_EQ(failed.front().second.type, SharedData::OperationErrorType::InvalidName);
        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        EXPECT_EQ(
            std::distance(
                std::filesystem::directory_iterator{localRoot.parent_path()}, std::filesystem::directory_iterator{}
            ),
            1
        );
    }

    TEST_F(BulkDownloadOperationTests, NeverWritesThroughALinkInsideTheLocalTree)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto outside = downloadDir_.path() / "outside";
        const auto localRoot = downloadDir_.path() / "linked";
        std::filesystem::create_directories(outside);
        std::filesystem::create_directories(localRoot);
#ifdef _WIN32
        const auto command =
            "cmd /c mklink /J \"" + (localRoot / "Documents").string() + "\" \"" + outside.string() + "\" >nul";
        ASSERT_EQ(std::system(command.c_str()), 0);
#else
        std::filesystem::create_directory_symlink(outside, localRoot / "Documents");
#endif

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{.type = SharedData::FileType::Directory});
        entries.push_back(
            SharedData::DirectoryEntry{.name = "Documents", .type = SharedData::FileType::Directory, .parent = 0}
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "doc1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Document 1 content"}.size(),
                .parent = 1,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{*sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot)}};
        operation.setScanResult(std::move(entries), 0);
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_empty(outside)) << "nothing may be written through the link";
        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 2u);
        for (auto const& [source, error] : failed)
            EXPECT_EQ(error.type, SharedData::OperationErrorType::LinkInLocalPath) << source;
    }

    TEST_F(BulkDownloadOperationTests, FailFastAbortsWholeOperationOnRecoverableError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "failfast";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "does_not_exist.txt",
                .type = SharedData::FileType::Regular,
                .size = 10,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot), .failFast = true}
        };
        operation.setScanResult(std::move(entries), 0);

        std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> workResult{
            BulkDownloadOperation::WorkStatus::MoreWork
        };
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
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "weird",
                .type = SharedData::FileType::Fifo,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{*sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot)}};
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
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "link_to_file1.txt",
                .type = SharedData::FileType::Symlink,
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{*sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot)}};
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
                    [&callbackInvocations](SharedData::BulkProgress const&)
                {
                    ++callbackInvocations;
                },
                .remotePath = remote("/home/test/Documents"),
                .localPath = toLocal(downloadDir_.path() / "progress"),
            }
        };
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_GT(callbackInvocations, 0);
    }

    TEST_F(BulkDownloadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        BulkDownloadOperation operation{
            *sftp,
            {.remotePath = remote("/home/test/Documents"), .localPath = toLocal(downloadDir_.path() / "completed")}
        };
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

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test/file1.txt"), .localPath = toLocal(downloadDir_.path() / "failed")}
        };

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "/home/test/file1.txt",
                .type = SharedData::FileType::Regular,
            }
        );
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
            .source = remote("/home/test/Documents/doc1.txt"),
            .destination = toLocal(localRoot / "doc1.txt"),
            .sizeBytes = std::string{"Document 1 content"}.size(),
        });
        files.push_back({
            .source = remote("/home/test/Documents/doc2.txt"),
            .destination = toLocal(localRoot / "doc2.txt"),
            .sizeBytes = std::string{"Document 2 content"}.size(),
        });

        BulkDownloadOperation operation{*sftp, {.localPath = toLocal(localRoot)}};
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
        ASSERT_TRUE(std::filesystem::exists(localRoot / "doc2.txt"));
        EXPECT_EQ(readLocalFile(localRoot / "doc1.txt"), "Document 1 content");
        EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkDownloadOperationTests, PrescannedFilesReplaceOnlyApprovedDestinations)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "approval";
        std::filesystem::create_directories(localRoot);
        // doc1.txt appeared after the existence check, doc2.txt existed and the user agreed to replace it.
        std::ofstream{localRoot / "doc1.txt"} << "appeared";
        std::ofstream{localRoot / "doc2.txt"} << "approved";

        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .source = remote("/home/test/Documents/doc1.txt"),
            .destination = toLocal(localRoot / "doc1.txt"),
            .sizeBytes = std::string{"Document 1 content"}.size(),
        });
        files.push_back({
            .source = remote("/home/test/Documents/doc2.txt"),
            .destination = toLocal(localRoot / "doc2.txt"),
            .sizeBytes = std::string{"Document 2 content"}.size(),
            .mayOverwrite = true,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = toLocal(localRoot)}};
        operation.setPrescannedFileList(std::move(files));
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_EQ(readLocalFile(localRoot / "doc1.txt"), "appeared");
        EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_EQ(failed.front().second.type, SharedData::OperationErrorType::FileExists);
    }

    TEST_F(BulkDownloadOperationTests, SetPrescannedFileListPreservesMtimeOnDownloadedFiles)
    {
        // Regression: BulkDownloadEntry → PrescannedFile → DirectoryEntry must
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
            .source = remote("/home/test/Documents/doc1.txt"),
            .destination = toLocal(localRoot / "doc1.txt"),
            .sizeBytes = std::string{"Document 1 content"}.size(),
            .mtime = mtimeDoc1,
            .mtimeNsec = 0,
        });
        files.push_back({
            .source = remote("/home/test/Documents/doc2.txt"),
            .destination = toLocal(localRoot / "doc2.txt"),
            .sizeBytes = std::string{"Document 2 content"}.size(),
            .mtime = mtimeDoc2,
            .mtimeNsec = 0,
        });

        BulkDownloadOperation operation{*sftp, {.localPath = toLocal(localRoot)}};
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        const auto mtimeSeconds = [](std::filesystem::path const& path)
        {
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

    TEST_F(BulkDownloadOperationTests, SetPrescannedFileListCreatesMissingLocalParents)
    {
        // Sync plans list files of directories that only exist remotely.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "prescanned_parents";
        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .source = remote("/home/test/Documents/doc1.txt"),
            .destination = toLocal(localRoot / "new" / "deeper" / "doc1.txt"),
            .sizeBytes = std::string{"Document 1 content"}.size(),
        });

        BulkDownloadOperation::BulkDownloadOperationOptions options{.localPath = toLocal(localRoot)};
        options.individualOptions.createMissingDirectories = true;
        BulkDownloadOperation operation{*sftp, std::move(options)};
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_EQ(readLocalFile(localRoot / "new" / "deeper" / "doc1.txt"), "Document 1 content");
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkDownloadOperationTests, UnwritableLocalTargetIsCollectedAndRestContinue)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "prescanned_unwritable";
        std::filesystem::create_directories(localRoot);
        {
            // A regular file where the first entry needs a directory.
            std::ofstream blocker{localRoot / "blocker"};
            blocker << "x";
        }

        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .source = remote("/home/test/Documents/doc1.txt"),
            .destination = toLocal(localRoot / "blocker" / "doc1.txt"),
            .sizeBytes = std::string{"Document 1 content"}.size(),
        });
        files.push_back({
            .source = remote("/home/test/Documents/doc2.txt"),
            .destination = toLocal(localRoot / "doc2.txt"),
            .sizeBytes = std::string{"Document 2 content"}.size(),
        });

        for (const bool createMissing : {false, true})
        {
            std::filesystem::remove(localRoot / "doc2.txt");
            BulkDownloadOperation::BulkDownloadOperationOptions options{.localPath = toLocal(localRoot)};
            options.individualOptions.createMissingDirectories = createMissing;
            BulkDownloadOperation operation{*sftp, std::move(options)};
            operation.setPrescannedFileList(files);

            ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
            EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
            EXPECT_EQ(operation.getFailed().size(), 1u);
        }
    }

    TEST_F(BulkDownloadOperationTests, CancelledOperationCannotBeWorkedFurther)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runScan(*sftp, "/home/test/Documents");

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test/Documents"), .localPath = toLocal(downloadDir_.path() / "cancel")}
        };
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);

        auto firstWork = operation.work();
        ASSERT_TRUE(firstWork.has_value());

        ASSERT_TRUE(operation.cancel(true).has_value());

        auto afterCancel = operation.work();
        ASSERT_FALSE(afterCancel.has_value());
        EXPECT_EQ(afterCancel.error().type, BulkDownloadOperation::ErrorType::CannotWorkCanceledOperation);
    }

    TEST_F(BulkDownloadOperationTests, ConcurrentPrescannedDownloadsAllFiles)
    {
        // Several files in flight at once must still all land with their original content.
        for (const int concurrency : {2, 3})
        {
            CREATE_SERVER_AND_JOINER(sftpServer);
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            const auto localRoot = downloadDir_.path() / fmt::format("concurrent_{}", concurrency);
            std::filesystem::create_directories(localRoot);

            std::vector<BulkDownloadOperation::PrescannedFile> files;
            files.push_back({
                .source = remote("/home/test/Documents/doc1.txt"),
                .destination = toLocal(localRoot / "doc1.txt"),
                .sizeBytes = std::string{"Document 1 content"}.size(),
            });
            files.push_back({
                .source = remote("/home/test/Documents/doc2.txt"),
                .destination = toLocal(localRoot / "doc2.txt"),
                .sizeBytes = std::string{"Document 2 content"}.size(),
            });
            files.push_back({
                .source = remote("/home/test/file1.txt"),
                .destination = toLocal(localRoot / "file1.txt"),
                .sizeBytes = std::string{"Fake file content"}.size(),
            });

            BulkDownloadOperation operation{*sftp, {.localPath = toLocal(localRoot), .concurrency = concurrency}};
            operation.setPrescannedFileList(std::move(files));

            ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

            EXPECT_EQ(readLocalFile(localRoot / "doc1.txt"), "Document 1 content");
            EXPECT_EQ(readLocalFile(localRoot / "doc2.txt"), "Document 2 content");
            EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
            EXPECT_TRUE(operation.getFailed().empty());
            EXPECT_EQ(operation.completedCount(), 3u);
            EXPECT_EQ(operation.inFlightCount(), 0u);
        }
    }

    TEST_F(BulkDownloadOperationTests, DirectoriesAreCreatedBeforeTheirFilesWithConcurrency)
    {
        // A nested tree with more files than slots: every directory must exist by the time
        // its files start, and every file must land.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "tree_concurrent";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "Documents",
                .type = SharedData::FileType::Directory,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "doc1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Document 1 content"}.size(),
                .parent = 2,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "doc2.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Document 2 content"}.size(),
                .parent = 2,
            }
        );

        BulkDownloadOperation operation{
            *sftp, {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot), .concurrency = 3}
        };
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(std::filesystem::is_directory(localRoot / "Documents"));
        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        EXPECT_EQ(readLocalFile(localRoot / "Documents" / "doc1.txt"), "Document 1 content");
        EXPECT_EQ(readLocalFile(localRoot / "Documents" / "doc2.txt"), "Document 2 content");
        EXPECT_TRUE(operation.getFailed().empty());
    }

    TEST_F(BulkDownloadOperationTests, FailingEntryIsCollectedAndRestContinueWithConcurrency)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "partial_concurrent";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "does_not_exist.txt",
                .type = SharedData::FileType::Regular,
                .size = 10,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{
            *sftp,
            {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot), .failFast = false, .concurrency = 2}
        };
        operation.setScanResult(std::move(entries), 0);

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_EQ(readLocalFile(localRoot / "file1.txt"), "Fake file content");
        EXPECT_FALSE(std::filesystem::exists(localRoot / "does_not_exist.txt"));

        const auto failed = operation.getFailed();
        ASSERT_EQ(failed.size(), 1u);
        EXPECT_TRUE(failed.front().first.ends_with("/does_not_exist.txt")) << failed.front().first;
    }

    TEST_F(BulkDownloadOperationTests, FailFastAbortsWholeOperationWithConcurrency)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "failfast_concurrent";

        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(
            SharedData::DirectoryEntry{
                .type = SharedData::FileType::Directory,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "does_not_exist.txt",
                .type = SharedData::FileType::Regular,
                .size = 10,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "file1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Fake file content"}.size(),
                .parent = 0,
            }
        );

        BulkDownloadOperation operation{
            *sftp,
            {.remotePath = remote("/home/test"), .localPath = toLocal(localRoot), .failFast = true, .concurrency = 2}
        };
        operation.setScanResult(std::move(entries), 0);

        std::expected<BulkDownloadOperation::WorkStatus, BulkDownloadOperation::Error> workResult{
            BulkDownloadOperation::WorkStatus::MoreWork
        };
        for (int iteration = 0; iteration < 500; ++iteration)
        {
            workResult = operation.work();
            if (!workResult.has_value())
                break;
            if (workResult.value() == BulkDownloadOperation::WorkStatus::Complete)
                break;
        }

        ASSERT_FALSE(workResult.has_value()) << "failFast must propagate the error out of work()";
        EXPECT_EQ(operation.state(), SharedData::OperationState::Failed);
        EXPECT_EQ(operation.inFlightCount(), 0u) << "failing the bulk must cancel the sibling transfers";
        ASSERT_EQ(operation.getFailed().size(), 1u);
    }

    TEST_F(BulkDownloadOperationTests, ProgressReportsInFlightBoundedAndCompletedMonotonic)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "progress_concurrent";
        std::filesystem::create_directories(localRoot);

        // large.txt (64 KiB) takes the async path and needs more than one work step, so both
        // slots are observably busy at once; a tiny file starts and finishes within one step
        // and never shows up as in flight.
        constexpr std::uint64_t largeSize = 65536u;
        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .source = remote("/home/test/large.txt"),
            .destination = toLocal(localRoot / "large_a.txt"),
            .sizeBytes = largeSize,
        });
        files.push_back({
            .source = remote("/home/test/large.txt"),
            .destination = toLocal(localRoot / "large_b.txt"),
            .sizeBytes = largeSize,
        });
        files.push_back({
            .source = remote("/home/test/file1.txt"),
            .destination = toLocal(localRoot / "file1.txt"),
            .sizeBytes = std::string{"Fake file content"}.size(),
        });

        constexpr int concurrency = 2;
        std::size_t maxInFlight = 0;
        std::uint64_t lastCompleted = 0;
        bool monotonic = true;
        SharedData::BulkProgress last{};
        BulkDownloadOperation operation{
            *sftp,
            {
                .overallProgressCallback =
                    [&](SharedData::BulkProgress const& update)
                {
                    maxInFlight = std::max(maxInFlight, update.inFlight.size());
                    monotonic = monotonic && update.fileCurrentIndex >= lastCompleted;
                    lastCompleted = update.fileCurrentIndex;
                    last = update;
                },
                .localPath = toLocal(localRoot),
                .concurrency = concurrency,
                .progressEmitInterval = std::chrono::milliseconds{0},
            }
        };
        operation.setPrescannedFileList(std::move(files));

        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(monotonic);
        EXPECT_LE(maxInFlight, static_cast<std::size_t>(concurrency));
        EXPECT_EQ(maxInFlight, static_cast<std::size_t>(concurrency)) << "both slots must be used at some point";
        EXPECT_EQ(last.fileCurrentIndex, 3u);
        EXPECT_EQ(last.fileCount, 3u);
        EXPECT_EQ(last.bytesCurrent, last.bytesTotal);
        EXPECT_TRUE(last.inFlight.empty());
    }

    TEST_F(BulkDownloadOperationTests, ParallelBudgetIsClampedToConcurrencyAndPendingWork)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localRoot = downloadDir_.path() / "budget";
        std::filesystem::create_directories(localRoot);

        constexpr std::uint64_t largeSize = 65536u;
        std::vector<BulkDownloadOperation::PrescannedFile> files;
        files.push_back({
            .source = remote("/home/test/large.txt"),
            .destination = toLocal(localRoot / "large_a.txt"),
            .sizeBytes = largeSize,
        });
        files.push_back({
            .source = remote("/home/test/large.txt"),
            .destination = toLocal(localRoot / "large_b.txt"),
            .sizeBytes = largeSize,
        });

        std::size_t maxInFlight = 0;
        BulkDownloadOperation operation{
            *sftp,
            {
                .overallProgressCallback =
                    [&maxInFlight](SharedData::BulkProgress const& update)
                {
                    maxInFlight = std::max(maxInFlight, update.inFlight.size());
                },
                .localPath = toLocal(localRoot),
                .concurrency = 3,
                .progressEmitInterval = std::chrono::milliseconds{0},
            }
        };
        operation.setPrescannedFileList(std::move(files));

        EXPECT_EQ(operation.parallelWorkDoable(1), 1);
        EXPECT_EQ(operation.parallelWorkDoable(10), 2) << "bounded by pending work";
        EXPECT_EQ(operation.parallelWorkDoable(0), 1) << "never below one";

        // The queue may grant fewer slots than the configured concurrency.
        operation.setParallelBudget(0);
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);
        EXPECT_EQ(maxInFlight, 1u);
        EXPECT_EQ(operation.parallelWorkDoable(10), 1) << "nothing pending, still answers at least one";
    }

#ifndef _WIN32
    TEST_F(BulkDownloadOperationTests, InheritedPermissionsNeverMakeTheRootWorldWritableOrSpecial)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // The scan root carries no permissions, applying "unknown" used to chmod it to 07777.
        const auto localRoot = downloadDir_.path() / "inherited";
        BulkDownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/Documents"),
                .localPath = toLocal(localRoot),
                .individualOptions = {.inheritPermissions = true},
            }
        };
        auto scan = runScan(*sftp, "/home/test/Documents");
        operation.setScanResult(std::move(scan.entries), scan.totalBytes);
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        const auto mode = std::filesystem::status(localRoot).permissions();
        EXPECT_EQ(mode & (std::filesystem::perms::set_uid | std::filesystem::perms::set_gid | std::filesystem::perms::sticky_bit), std::filesystem::perms::none);
        EXPECT_EQ(mode & std::filesystem::perms::others_write, std::filesystem::perms::none);
        EXPECT_TRUE(std::filesystem::exists(localRoot / "doc1.txt"));
    }

    TEST_F(BulkDownloadOperationTests, AReadOnlyDirectoryStillReceivesItsContent)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        using std::filesystem::perms;
        const auto readOnly = perms::owner_read | perms::owner_exec | perms::group_read | perms::group_exec |
            perms::others_read | perms::others_exec;
        std::vector<SharedData::DirectoryEntry> entries;
        entries.push_back(SharedData::DirectoryEntry{.type = SharedData::FileType::Directory});
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "Documents",
                .type = SharedData::FileType::Directory,
                .permissions = readOnly,
                .parent = 0,
            }
        );
        entries.push_back(
            SharedData::DirectoryEntry{
                .name = "doc1.txt",
                .type = SharedData::FileType::Regular,
                .size = std::string{"Document 1 content"}.size(),
                .parent = 1,
            }
        );

        const auto localRoot = downloadDir_.path() / "readonly";
        auto restore = Nui::ScopeExit{[&localRoot]() noexcept {
            std::error_code error{};
            std::filesystem::permissions(localRoot / "Documents", perms::owner_all, error);
        }};
        BulkDownloadOperation operation{
            sftp,
            {
                .remotePath = remote("/home/test"),
                .localPath = toLocal(localRoot),
                .individualOptions = {.inheritPermissions = true},
            }
        };
        operation.setScanResult(std::move(entries), std::string{"Document 1 content"}.size());
        ASSERT_EQ(runBulkToCompletion(operation), BulkDownloadOperation::WorkStatus::Complete);

        EXPECT_TRUE(operation.getFailed().empty());
        EXPECT_EQ(readLocalFile(localRoot / "Documents" / "doc1.txt"), "Document 1 content");
        EXPECT_EQ(std::filesystem::status(localRoot / "Documents").permissions(), readOnly & ~Utility::processUmask());
    }
#endif
}
