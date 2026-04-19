#pragma once

#include <backend/sftp/archive_download_operation.hpp>
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

        const auto archivePath = archiveDir_.path() / "documents.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeDirectoryEntry("/home/test/Documents")},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        // Directory header + doc1.txt + doc2.txt.
        EXPECT_NE(findEntry(entries, "Documents/doc1.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "Documents/doc2.txt"), nullptr);

        const auto* doc1 = findEntry(entries, "Documents/doc1.txt");
        ASSERT_NE(doc1, nullptr);
        EXPECT_EQ(doc1->contents, "Document 1 content");
    }

    TEST_F(ArchiveDownloadOperationTests, LargeFilePassesThrough)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto archivePath = archiveDir_.path() / "large.tar";
        ArchiveDownloadOperation operation{
            *sftp,
            {
                .entries = {makeFileEntry("/home/test/large.txt", 1024u * 1024u)},
                .localArchivePath = archivePath,
                .compression = TarArchive::Compression::None,
            }};
        // 1 MiB / 64 KiB chunk = 16 reads, plus header/finalise work.  A
        // generous budget keeps the test robust to strand scheduling jitter.
        ASSERT_TRUE(runToCompletion(operation, 2000).has_value());

        const auto entries = recoverArchiveEntries(archivePath);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].path, "large.txt");
        EXPECT_EQ(entries[0].contents.size(), 1024u * 1024u);
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
