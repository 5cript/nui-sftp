#pragma once

#include <backend/sftp/archive_upload_operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include "real_server_tests.hpp"
#include "archive_test_helpers.hpp"

#include <tar_archive/compression.hpp>

#include <nui/utility/scope_exit.hpp>
#include <utility/temporary_directory.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace Test
{
    class ArchiveUploadOperationTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            RealServerTests::SetUp();

            writeFile(srcDir_.path() / "alpha.txt", "alpha payload");
            writeFile(srcDir_.path() / "beta.txt", "beta payload");

            std::filesystem::create_directories(srcDir_.path() / "nested");
            writeFile(srcDir_.path() / "nested" / "deep.txt", "deep payload");
        }

      protected:
        Utility::TemporaryDirectory srcDir_{programDirectory / "temp", true};
        Utility::TemporaryDirectory pulledDir_{programDirectory / "temp", true};

        static void writeFile(std::filesystem::path const& path, std::string_view contents)
        {
            std::ofstream stream{path, std::ios::binary};
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        }

        static std::expected<ArchiveUploadOperation::WorkStatus, ArchiveUploadOperation::Error>
        runToCompletion(ArchiveUploadOperation& operation, int maxIterations = 500)
        {
            std::expected<ArchiveUploadOperation::WorkStatus, ArchiveUploadOperation::Error> last{};
            for (int iteration = 0; iteration < maxIterations; ++iteration)
            {
                last = operation.work();
                if (!last.has_value())
                    return last;
                if (last.value() == ArchiveUploadOperation::WorkStatus::Complete)
                    return last;
            }
            return last;
        }

        /**
         * @brief Run a LocalScanOperation to completion and return the walker
         *        entry vector plus cumulative totalBytes, matching what the
         *        OperationQueue dispatcher hands to setScanResultForRoot.
         */
        struct LocalScanBundle
        {
            std::vector<SharedData::DirectoryEntry> entries{};
            std::uint64_t totalBytes{0u};
        };
        static LocalScanBundle runLocalScan(std::filesystem::path const& path)
        {
            LocalScanOperation scan{
                LocalScanOperation::ScanOperationOptions{.localPath = path},
            };
            while (true)
            {
                const auto step = scan.work();
                EXPECT_TRUE(step.has_value()) << "local scan failed";
                if (!step.has_value() || step.value() == LocalScanOperation::WorkStatus::Complete)
                    break;
            }
            const auto totalBytes = scan.totalBytes();
            return LocalScanBundle{.entries = scan.ejectEntries(), .totalBytes = totalBytes};
        }

        /**
         * @brief Pull the just-uploaded archive from the remote side back to
         *        local disk so the test can hand the bytes to a
         *        TarArchive::Reader. The remote archive is the thing we're
         *        asserting on; this helper is only about transport.
         */
        std::filesystem::path
        pullRemoteArchive(
            SecureShell::SftpSession& sftp,
            std::filesystem::path const& remotePath,
            std::string const& localFilename
        )
        {
            const auto localPath = pulledDir_.path() / localFilename;
            DownloadOperation download{
                sftp,
                {
                    .remotePath = remotePath,
                    .localPath = localPath,
                    .mayOverwrite = true,
                }};
            for (int i = 0; i < 500; ++i)
            {
                auto result = download.work();
                EXPECT_TRUE(result.has_value()) << "DownloadOperation failed while pulling archive";
                if (!result.has_value())
                    break;
                if (result.value() == DownloadOperation::WorkStatus::Complete)
                    break;
            }
            return localPath;
        }
    };

    TEST_F(ArchiveUploadOperationTests, CanCreateServer)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
    }

    TEST_F(ArchiveUploadOperationTests, TypeIsArchiveUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        ArchiveUploadOperation operation{*sftp, {}};
        EXPECT_EQ(operation.type(), SharedData::OperationType::ArchiveUpload);
    }

    TEST_F(ArchiveUploadOperationTests, EmptyRemotePathFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = {},
            }};
        auto result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveUploadOperation::ErrorType::InvalidPath);
    }

    TEST_F(ArchiveUploadOperationTests, NoLocalPathsFails)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {},
                .remoteArchivePath = "/home/test/nothing.tar",
            }};
        auto result = operation.work();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveUploadOperation::ErrorType::InvalidPath);
    }

    TEST_F(ArchiveUploadOperationTests, SingleFileUncompressedRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const std::filesystem::path remoteArchive = "/home/test/single.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "single.tar");
        const auto entries = recoverArchiveEntries(pulled);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].path, "alpha.txt");
        EXPECT_EQ(entries[0].contents, "alpha payload");
    }

    TEST_F(ArchiveUploadOperationTests, SingleFileGzipRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const std::filesystem::path remoteArchive = "/home/test/alpha.tar.gz";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = remoteArchive,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "alpha.tar.gz");
        const auto entries = recoverArchiveEntries(pulled);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "alpha payload");
    }

    TEST_F(ArchiveUploadOperationTests, SingleFileZstdRoundTrip)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const std::filesystem::path remoteArchive = "/home/test/alpha.tar.zst";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = remoteArchive,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "alpha.tar.zst");
        const auto entries = recoverArchiveEntries(pulled);
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].contents, "alpha payload");
    }

    TEST_F(ArchiveUploadOperationTests, MultipleLocalFilesInOneArchive)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const std::filesystem::path remoteArchive = "/home/test/pair.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths =
                    {
                        srcDir_.path() / "alpha.txt",
                        srcDir_.path() / "beta.txt",
                    },
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "pair.tar");
        const auto entries = recoverArchiveEntries(pulled);
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_NE(findEntry(entries, "alpha.txt"), nullptr);
        EXPECT_NE(findEntry(entries, "beta.txt"), nullptr);

        const auto* alpha = findEntry(entries, "alpha.txt");
        ASSERT_NE(alpha, nullptr);
        EXPECT_EQ(alpha->contents, "alpha payload");
    }

    TEST_F(ArchiveUploadOperationTests, LocalDirectoryIsFlattenedRecursively)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(srcDir_.path());

        const std::filesystem::path remoteArchive = "/home/test/tree.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path()},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        operation.setScanResultForRoot(srcDir_.path(), std::move(scan.entries), scan.totalBytes);
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "tree.tar");
        const auto entries = recoverArchiveEntries(pulled);

        const std::string rootName = srcDir_.path().filename().generic_string();
        EXPECT_NE(findEntry(entries, rootName + "/alpha.txt"), nullptr);
        EXPECT_NE(findEntry(entries, rootName + "/beta.txt"), nullptr);
        EXPECT_NE(findEntry(entries, rootName + "/nested/deep.txt"), nullptr);

        const auto* deep = findEntry(entries, rootName + "/nested/deep.txt");
        ASSERT_NE(deep, nullptr);
        EXPECT_EQ(deep->contents, "deep payload");
    }

    TEST_F(ArchiveUploadOperationTests, DirectoryRootWithoutPrescanErrors)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path()},
                .remoteArchivePath = "/home/test/missing_scan.tar",
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        // No setScanResultForRoot — the op must not self-recurse; it has to
        // surface an ImplementationError like its download counterpart.
        const auto result = runToCompletion(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveUploadOperation::ErrorType::ImplementationError);
    }

    TEST_F(ArchiveUploadOperationTests, UnrelatedPrescanRootStillFailsMatchingDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // Create a second directory tree distinct from srcDir_.  The scan is
        // keyed on that path; the archive asks to pack srcDir_ instead, so
        // the real root is still missing its scan and must error.
        Utility::TemporaryDirectory otherDir{programDirectory / "temp", true};
        writeFile(otherDir.path() / "unrelated.txt", "unrelated");
        auto otherScan = runLocalScan(otherDir.path());

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path()},
                .remoteArchivePath = "/home/test/wrong_key.tar",
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        operation.setScanResultForRoot(
            otherDir.path(), std::move(otherScan.entries), otherScan.totalBytes
        );
        const auto result = runToCompletion(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, ArchiveUploadOperation::ErrorType::ImplementationError);
    }

    TEST_F(ArchiveUploadOperationTests, MultipleDirectoryRootsAggregateUnderBasenames)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        Utility::TemporaryDirectory firstDir{programDirectory / "temp", true};
        Utility::TemporaryDirectory secondDir{programDirectory / "temp", true};
        writeFile(firstDir.path() / "one.txt", "one payload");
        writeFile(secondDir.path() / "two.txt", "two payload");

        auto firstScan = runLocalScan(firstDir.path());
        auto secondScan = runLocalScan(secondDir.path());

        const std::filesystem::path remoteArchive = "/home/test/two_roots_up.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {firstDir.path(), secondDir.path()},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        operation.setScanResultForRoot(
            firstDir.path(), std::move(firstScan.entries), firstScan.totalBytes
        );
        operation.setScanResultForRoot(
            secondDir.path(), std::move(secondScan.entries), secondScan.totalBytes
        );
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "two_roots_up.tar");
        const auto entries = recoverArchiveEntries(pulled);
        const auto firstName = firstDir.path().filename().generic_string();
        const auto secondName = secondDir.path().filename().generic_string();
        EXPECT_NE(findEntry(entries, firstName + "/one.txt"), nullptr);
        EXPECT_NE(findEntry(entries, secondName + "/two.txt"), nullptr);
    }

    TEST_F(ArchiveUploadOperationTests, MixedRegularAndDirectoryRoots)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        auto scan = runLocalScan(srcDir_.path());

        const std::filesystem::path topFile = srcDir_.path() / "alpha.txt";
        const std::filesystem::path remoteArchive = "/home/test/mixed_up.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                // A regular-file root (no scan needed) alongside a directory
                // root (which does need a scan) — the op must succeed and
                // must not mis-flag the file as missing-scan.
                .localPaths = {topFile, srcDir_.path()},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        operation.setScanResultForRoot(srcDir_.path(), std::move(scan.entries), scan.totalBytes);
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "mixed_up.tar");
        const auto entries = recoverArchiveEntries(pulled);

        const auto rootName = srcDir_.path().filename().generic_string();
        EXPECT_NE(findEntry(entries, "alpha.txt"), nullptr);
        EXPECT_NE(findEntry(entries, rootName + "/beta.txt"), nullptr);
    }

    TEST_F(ArchiveUploadOperationTests, NestedSubdirectoryPreservesRelativePath)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // srcDir_ already contains `nested/deep.txt` via SetUp(); this is
        // the upload-side fullPathRelative check — a grandchild must land
        // at <root>/nested/deep.txt, not <root>/deep.txt.
        auto scan = runLocalScan(srcDir_.path());

        const std::filesystem::path remoteArchive = "/home/test/nested_up.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path()},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        operation.setScanResultForRoot(srcDir_.path(), std::move(scan.entries), scan.totalBytes);
        ASSERT_TRUE(runToCompletion(operation).has_value());

        const auto pulled = pullRemoteArchive(*sftp, remoteArchive, "nested_up.tar");
        const auto entries = recoverArchiveEntries(pulled);
        const auto rootName = srcDir_.path().filename().generic_string();

        const auto* deep = findEntry(entries, rootName + "/nested/deep.txt");
        ASSERT_NE(deep, nullptr);
        EXPECT_EQ(deep->contents, "deep payload");
    }

    TEST_F(ArchiveUploadOperationTests, WorkOnCompletedOperationReturnsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = "/home/test/done.tar",
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        auto again = operation.work();
        ASSERT_FALSE(again.has_value());
        EXPECT_EQ(again.error().type, ArchiveUploadOperation::ErrorType::CannotWorkCompletedOperation);
    }

    TEST_F(ArchiveUploadOperationTests, ProgressCallbackReachesPlannedBytes)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        std::uint64_t lastCurrent{0u};
        std::uint64_t lastMax{0u};

        ArchiveUploadOperation operation{
            *sftp,
            {
                .progressCallback =
                    [&](std::uint64_t, std::uint64_t max, std::uint64_t current, std::make_signed_t<std::size_t>)
                    {
                        lastMax = max;
                        lastCurrent = current;
                    },
                .localPaths = {srcDir_.path() / "alpha.txt"},
                .remoteArchivePath = "/home/test/progress.tar",
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
        ASSERT_TRUE(runToCompletion(operation).has_value());

        EXPECT_EQ(lastMax, std::string{"alpha payload"}.size());
        EXPECT_EQ(lastCurrent, std::string{"alpha payload"}.size());
    }
}
