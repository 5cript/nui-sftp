#pragma once

#include <backend/sftp/archive_download_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include "real_server_tests.hpp"
#include "archive_test_helpers.hpp"

#include <nui/utility/scope_exit.hpp>
#include <utility/temporary_directory.hpp>

#include <cstdint>
#include <fstream>

namespace Test
{
    class ArchiveDownloadOperationTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            RealServerTests::SetUp();
        }

      protected:
        Utility::TemporaryDirectory archiveDir_{programDirectory / "temp", true};

        /**
         * @brief Drive an ArchiveDownloadOperation to completion or until it errors.
         *        Returns the last work result so the caller can still inspect errors.
         */
        static std::expected<ArchiveDownloadOperation::WorkStatus, ArchiveDownloadOperation::Error>
        runToCompletion(ArchiveDownloadOperation& operation, int maxIterations = 500)
        {
            std::expected<ArchiveDownloadOperation::WorkStatus, ArchiveDownloadOperation::Error> last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                last = operation.work();
                if (!last.has_value())
                    return last;
                if (last.value() == ArchiveDownloadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        /**
         * @brief Run a remote ScanOperation to completion and return its flat
         *        walker entry vector plus cumulative totalBytes, matching what
         *        OperationQueue's dispatcher hands to setScanResultForRoot.
         */
        struct ScanBundle
        {
            std::vector<SharedData::DirectoryEntry> entries{};
            std::uint64_t totalBytes{0u};
        };
        static ScanBundle runScan(SecureShell::SftpSession& sftp, std::filesystem::path const& path)
        {
            ScanOperation scan{
                sftp,
                ScanOperation::ScanOperationOptions{.remotePath = path},
            };
            while (true)
            {
                const auto step = scan.work();
                EXPECT_TRUE(step.has_value()) << "scan failed";
                if (!step.has_value() || step.value() == ScanOperation::WorkStatus::Complete)
                    break;
            }
            const auto totalBytes = scan.totalBytes();
            return ScanBundle{.entries = std::move(scan).ejectEntries(), .totalBytes = totalBytes};
        }
    };

    TEST_F(ArchiveDownloadOperationTests, CanCreateServer)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
    }

    TEST_F(ArchiveDownloadOperationTests, TypeIsArchiveDownload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ArchiveDownloadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::ArchiveDownload);
    }

    TEST_F(ArchiveDownloadOperationTests, EmptyLocalArchivePathFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = {},
            }};
        auto result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveDownloadOperation::ErrorType::InvalidPath);
    }

    TEST_F(ArchiveDownloadOperationTests, NoEntriesFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {},
                .localArchivePath = archiveDir_.path() / "empty.tar",
            }};
        auto result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveDownloadOperation::ErrorType::ImplementationError);
    }

    TEST_F(ArchiveDownloadOperationTests, SingleFileUncompressedRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "one.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        auto result = runToCompletion(operation);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, ArchiveDownloadOperation::WorkStatus::Complete);

        ASSERT_TRUE(std::filesystem::exists(archivePath));
        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].path, "file1.txt");
        EXPECT_EQ(entries[0].type, SharedData::FileType::Regular);
        EXPECT_EQ(entries[0].contents, "Fake file content");
    }

    TEST_F(ArchiveDownloadOperationTests, SingleFileGzipRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "one.tar.gz";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                // Auto: detected from extension.
            }};
        auto result = runToCompletion(operation);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, ArchiveDownloadOperation::WorkStatus::Complete);

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "Fake file content");
    }

    TEST_F(ArchiveDownloadOperationTests, SingleFileBzip2RoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "one.tar.bz2";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "Fake file content");
    }

    TEST_F(ArchiveDownloadOperationTests, SingleFileZstdRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "one.tar.zst";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "Fake file content");
    }

    TEST_F(ArchiveDownloadOperationTests, MultipleFilesInArchive)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "pair.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries =
                    {
                        makeFileEntry("/home/test/file1.txt", 17u),
                        makeFileEntry("/home/test/file2.txt", 17u),
                    },
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_NE(findEntry(entries, "file1.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "file2.txt"), nullptr);
    }

    TEST_F(ArchiveDownloadOperationTests, DirectoryRecursionFlattensSubtree)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const std::filesystem::path rootPath = "/home/test/Documents";
        auto scan = runScan(*sftp, rootPath);

        const auto archivePath = archiveDir_.path() / "documents.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeDirectoryEntry(rootPath)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        operation.setScanResultForRoot(rootPath, std::move(scan.entries), scan.totalBytes);
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        EXPECT_NE(findEntry(entries, "Documents/doc1.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "Documents/doc2.txt"), nullptr);

        const auto* doc1 = findEntry(entries, "Documents/doc1.txt");
        ASSERT_NE(doc1, nullptr);
        EXPECT_EQ(doc1->contents, "Document 1 content");
    }

    TEST_F(ArchiveDownloadOperationTests, DirectoryRootWithoutPrescanErrors)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "missing_scan.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeDirectoryEntry("/home/test/Documents")},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        // Intentionally skip setScanResultForRoot — the op must not
        // self-recurse; it has to surface an ImplementationError instead.
        const auto result = runToCompletion(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveDownloadOperation::ErrorType::ImplementationError);
    }

    TEST_F(ArchiveDownloadOperationTests, UnrelatedPrescanRootStillFailsMatchingDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // Pre-scan Pictures but then ask the archive to pack Documents —
        // the setScanResultForRoot call is keyed by the mismatched path and
        // must not satisfy the Documents root, which is still missing.
        auto pictures = runScan(*sftp, "/home/test/Pictures");

        const auto archivePath = archiveDir_.path() / "wrong_key.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeDirectoryEntry("/home/test/Documents")},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        operation.setScanResultForRoot(
            "/home/test/Pictures", std::move(pictures.entries), pictures.totalBytes
        );
        const auto result = runToCompletion(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveDownloadOperation::ErrorType::ImplementationError);
    }

    TEST_F(ArchiveDownloadOperationTests, MultipleDirectoryRootsAggregateUnderBasenames)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto documents = runScan(*sftp, "/home/test/Documents");
        auto pictures = runScan(*sftp, "/home/test/Pictures");

        const auto archivePath = archiveDir_.path() / "two_roots.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries =
                    {
                        makeDirectoryEntry("/home/test/Documents"),
                        makeDirectoryEntry("/home/test/Pictures"),
                    },
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        operation.setScanResultForRoot(
            "/home/test/Documents", std::move(documents.entries), documents.totalBytes
        );
        operation.setScanResultForRoot(
            "/home/test/Pictures", std::move(pictures.entries), pictures.totalBytes
        );
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        EXPECT_NE(findEntry(entries, "Documents/doc1.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "Documents/doc2.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "Pictures/image1.png"), nullptr);
        EXPECT_NE(findEntry(entries, "Pictures/image2.jpg"), nullptr);
    }

    TEST_F(ArchiveDownloadOperationTests, MixedRegularAndDirectoryRoots)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto documents = runScan(*sftp, "/home/test/Documents");

        const auto archivePath = archiveDir_.path() / "mixed_roots.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries =
                    {
                        makeFileEntry("/home/test/file1.txt", 17u),
                        makeDirectoryEntry("/home/test/Documents"),
                    },
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        // Regular-file roots never receive a setScanResultForRoot call — only
        // the directory root does. This asserts the op still succeeds in that
        // mixed configuration and doesn't treat file1.txt as missing-scan.
        operation.setScanResultForRoot(
            "/home/test/Documents", std::move(documents.entries), documents.totalBytes
        );
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        const auto* file1 = findEntry(entries, "file1.txt");
        ASSERT_NE(file1, nullptr);
        EXPECT_EQ(file1->contents, "Fake file content");
        EXPECT_NE(findEntry(entries, "Documents/doc1.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "Documents/doc2.txt"), nullptr);
    }

    TEST_F(ArchiveDownloadOperationTests, NestedSubdirectoryPreservesRelativePath)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // /var contains the subdirectory 'log', which itself contains files.
        // This exercises the tar-path composition through fullPathRelative:
        // a deeper-than-one-level entry must come out as 'var/log/syslog',
        // not 'var/syslog'.
        auto scan = runScan(*sftp, "/var");

        const auto archivePath = archiveDir_.path() / "var_tree.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeDirectoryEntry("/var")},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        operation.setScanResultForRoot("/var", std::move(scan.entries), scan.totalBytes);
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        const auto* syslog = findEntry(entries, "var/log/syslog");
        ASSERT_NE(syslog, nullptr);
        EXPECT_EQ(syslog->contents, "Fake syslog content");
        const auto* authLog = findEntry(entries, "var/log/auth.log");
        ASSERT_NE(authLog, nullptr);
        EXPECT_EQ(authLog->contents, "Fake auth.log content");
    }

    TEST_F(ArchiveDownloadOperationTests, LargeFilePassesThrough)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "large.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/large.txt", 65536u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation, 2000).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].path, "large.txt");
        EXPECT_EQ(entries[0].contents.size(), 65536u);
    }

    TEST_F(ArchiveDownloadOperationTests, FilepartAbsentAfterCompletion)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "tmp_check.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        EXPECT_TRUE(std::filesystem::exists(archivePath));
        EXPECT_FALSE(std::filesystem::exists(archivePath.generic_string() + ".filepart"));
    }

    TEST_F(ArchiveDownloadOperationTests, RefusesToOverwriteExistingArchive)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "existing.tar";
        {
            std::ofstream sentinel{archivePath, std::ios::binary};
            sentinel << "existing";
        }

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                .mayOverwrite = false,
            }};
        auto result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveDownloadOperation::ErrorType::FileExists);
    }

    TEST_F(ArchiveDownloadOperationTests, OverwritesExistingArchiveWhenFlagSet)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "overwrite.tar";
        {
            std::ofstream sentinel{archivePath, std::ios::binary};
            sentinel << "not-a-real-tar";
        }

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "Fake file content");
    }

    TEST_F(ArchiveDownloadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archiveDir_.path() / "finished.tar",
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        auto again = operation.work();
        ASSERT_FALSE(again.has_value());
        EXPECT_EQ(again.error().type, ArchiveDownloadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(ArchiveDownloadOperationTests, ProgressCallbackReachesTotalBytes)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::uint64_t lastCurrent{0u};
        std::uint64_t lastMax{0u};

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .progressCallback =
                    [&](std::uint64_t, std::uint64_t max, std::uint64_t current, std::make_signed_t<std::size_t>)
                    {
                        lastMax = max;
                        lastCurrent = current;
                    },
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archiveDir_.path() / "with_progress.tar",
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        EXPECT_EQ(lastMax, 17u);
        EXPECT_EQ(lastCurrent, 17u);
    }

    TEST_F(ArchiveDownloadOperationTests, CreatesMissingLocalParentsWhenRequested)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "nested" / "deeper" / "packed.tar";
        ASSERT_FALSE(std::filesystem::exists(archivePath.parent_path()));

        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/file1.txt", 17u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
                .createMissingDirectories = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());
        EXPECT_TRUE(std::filesystem::exists(archivePath));
    }
}
