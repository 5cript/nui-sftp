#pragma once

#include <backend/sftp/archive_upload_operation.hpp>
#include <backend/sftp/download_operation.hpp>
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

        const std::filesystem::path remoteArchive = "/home/test/tree.tar";
        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {srcDir_.path()},
                .remoteArchivePath = remoteArchive,
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }};
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
