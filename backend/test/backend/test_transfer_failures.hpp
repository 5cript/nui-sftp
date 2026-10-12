#pragma once

#include <backend/sftp/archive_upload_operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include "real_server_tests.hpp"

#include <expected>
#include <fstream>
#include <string>
#include <thread>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class TransferFailureTests : public RealServerTests
    {
      public:
        static std::string alphabet(std::size_t length)
        {
            std::string result;
            result.reserve(length);
            for (std::size_t index = 0; index < length; ++index)
                result.push_back(static_cast<char>('a' + (index % 26)));
            return result;
        }

        static std::string readLocalFile(std::filesystem::path const& path)
        {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        static void writeLocalFile(std::filesystem::path const& path, std::string const& content)
        {
            std::ofstream file{path, std::ios::binary};
            file << content;
        }

        static std::string readRemoteFile(SecureShell::SftpSession& sftp, std::filesystem::path const& path)
        {
            auto openFuture = sftp.openFile(remote(path.generic_string()), SecureShell::SftpSession::OpenType::Read, std::filesystem::perms::unknown);
            if (openFuture.wait_for(5s) != std::future_status::ready)
                return "<open timed out>";
            auto openResult = openFuture.get();
            if (!openResult.has_value())
                return "<missing>";
            auto file = openResult->lock();
            std::string data;
            auto readFuture = file->readAll(
                [&data](std::string_view chunk)
                {
                    data.append(chunk);
                    return true;
                }
            );
            if (readFuture.wait_for(10s) != std::future_status::ready || !readFuture.get().has_value())
                return "<read failed>";
            return data;
        }

        /**
         * @brief Works an operation until it completes or fails. Async transfers run on the strand,
         *        so MoreWork is retried with a short pause.
         */
        template <typename OperationT>
        static std::expected<Operation::WorkStatus, Operation::Error> runToEnd(OperationT& operation)
        {
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto result = operation.work();
                if (!result.has_value() || result.value() == Operation::WorkStatus::Complete)
                    return result;
                std::this_thread::sleep_for(2ms);
            }
            return std::unexpected(Operation::Error{.type = Operation::ErrorType::FutureTimeout});
        }

      protected:
        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
    };

    TEST_F(TransferFailureTests, AsyncDownloadReadFailureKeepsExistingFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("failReadsAfter", {{"bytes", 20000}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "large.txt";
        writeLocalFile(localPath, "original content");

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/large.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .bigFileOptimized = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readLocalFile(localPath), "original content");
        EXPECT_FALSE(std::filesystem::exists(localPath.generic_string() + ".filepart"));
    }

    TEST_F(TransferFailureTests, AsyncDownloadConnectionDropKeepsExistingFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("dropAfterBytes", {{"bytes", 20000}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "large.txt";
        writeLocalFile(localPath, "original content");

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/large.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .bigFileOptimized = true,
                .futureTimeout = std::chrono::seconds{3},
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readLocalFile(localPath), "original content");
    }

    TEST_F(TransferFailureTests, SyncDownloadReadFailureKeepsExistingFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("failReadsAfter", {{"bytes", 0}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "file1.txt";
        writeLocalFile(localPath, "original content");

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readLocalFile(localPath), "original content");
    }

    TEST_F(TransferFailureTests, DownloadOfFileShorterThanListedKeepsExistingFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        for (const bool bigFileOptimized : {false, true})
        {
            const auto localPath = localDirectory_.path() / fmt::format("large_{}.txt", bigFileOptimized);
            writeLocalFile(localPath, "original content");

            SharedData::DirectoryEntry entry{};
            entry.name = "large.txt";
            entry.type = SharedData::FileType::Regular;
            entry.size = 70000u;

            DownloadOperation operation{
                *sftp,
                {
                    .remotePath = remote("/home/test/large.txt"),
                    .localPath = toLocal(localPath),
                    .mayOverwrite = true,
                    .bigFileOptimized = bigFileOptimized,
                    .entry = entry,
                }
            };

            const auto result = runToEnd(operation);
            ASSERT_FALSE(result.has_value()) << "bigFileOptimized: " << bigFileOptimized;
            EXPECT_EQ(readLocalFile(localPath), "original content") << "bigFileOptimized: " << bigFileOptimized;
        }
    }

    TEST_F(TransferFailureTests, AsyncDownloadResumesFromPartialFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "large.txt";
        writeLocalFile(localPath.generic_string() + ".filepart", alphabet(65536).substr(0, 20000));

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/large.txt"),
                .localPath = toLocal(localPath),
                .tryContinue = true,
                .bigFileOptimized = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readLocalFile(localPath), alphabet(65536));
    }

    TEST_F(TransferFailureTests, CompletePartialFileIsFinalizedWithoutReading)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "file1.txt";
        writeLocalFile(localPath.generic_string() + ".filepart", "Fake file content");

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .tryContinue = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(TransferFailureTests, FollowedSymlinkDownloadsTargetContent)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "followed.txt";
        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/link_to_file1.txt"),
                .localPath = toLocal(localPath),
                .symlinkHandling = Persistence::SymlinkHandling::FollowSymlink,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_FALSE(std::filesystem::is_symlink(std::filesystem::symlink_status(localPath)));
        EXPECT_EQ(readLocalFile(localPath), "Fake file content");
    }

    TEST_F(TransferFailureTests, AsyncUploadWriteFailureKeepsExistingRemoteFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", 20000}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "upload.txt";
        writeLocalFile(localPath, alphabet(65536));

        UploadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
                .bigFileOptimized = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "Fake file content");
    }

    TEST_F(TransferFailureTests, SyncUploadWriteFailureKeepsExistingRemoteFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", 0}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "upload.txt";
        writeLocalFile(localPath, "new content");

        UploadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
                .mayOverwrite = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "Fake file content");
    }

    TEST_F(TransferFailureTests, AsyncUploadResumesFromPartialRemoteFile)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        const auto content = alphabet(65536);
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/resumed.txt.filepart"}, {"content", content.substr(0, 20000)}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "resumed.txt";
        writeLocalFile(localPath, content);

        UploadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/resumed.txt"),
                .localPath = toLocal(localPath),
                .tryContinue = true,
                .bigFileOptimized = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/resumed.txt"), content);
    }

#ifndef _WIN32
    TEST_F(TransferFailureTests, DownloadRefusesASymlinkAtTheTempPath)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto outside = localDirectory_.path() / "outside.txt";
        writeLocalFile(outside, "must stay");
        const auto localPath = localDirectory_.path() / "file1.txt";
        std::filesystem::create_symlink(outside, localPath.generic_string() + ".filepart");

        DownloadOperation operation{
            *sftp,
            {
                .remotePath = remote("/home/test/file1.txt"),
                .localPath = toLocal(localPath),
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(readLocalFile(outside), "must stay");
    }
#endif

    TEST_F(TransferFailureTests, UploadOverwriteReplacesWithAndWithoutPosixRename)
    {
        for (const bool posixRename : {false, true})
        {
            CREATE_SERVER_AND_JOINER(sftpServer);
            if (posixRename)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            const auto localPath = localDirectory_.path() / "upload.txt";
            writeLocalFile(localPath, "new content");

            UploadOperation operation{
                *sftp,
                {
                    .remotePath = remote("/home/test/file1.txt"),
                    .localPath = toLocal(localPath),
                    .mayOverwrite = true,
                }
            };

            const auto result = runToEnd(operation);
            ASSERT_TRUE(result.has_value()) << result.error().toString();
            EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "new content");
            EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt.filepart.old"), "<missing>");
        }
    }

    TEST_F(TransferFailureTests, FailedUploadRenameKeepsExistingRemoteFile)
    {
        for (const bool posixRename : {false, true})
        {
            CREATE_SERVER_AND_JOINER(sftpServer);
            if (posixRename)
                ASSERT_TRUE(serverStartResult->command("advertise", {{"name", "posix-rename@openssh.com"}}));
            ASSERT_TRUE(serverStartResult->command("failRenames", {{"enabled", true}}));
            auto [_, sftp] = createSftpSession(serverStartResult->port);

            const auto localPath = localDirectory_.path() / "upload.txt";
            writeLocalFile(localPath, "new content");

            UploadOperation operation{
                *sftp,
                {
                    .remotePath = remote("/home/test/file1.txt"),
                    .localPath = toLocal(localPath),
                    .mayOverwrite = true,
                }
            };

            const auto result = runToEnd(operation);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(readRemoteFile(*sftp, "/home/test/file1.txt"), "Fake file content");
        }
    }

    TEST_F(TransferFailureTests, FailedArchiveUploadKeepsExistingRemoteArchive)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/backup.tar"}, {"content", "old archive"}}));
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", 0}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "packed.txt";
        writeLocalFile(localPath, alphabet(4096));

        ArchiveUploadOperation operation{
            *sftp,
            {
                .localPaths = {toLocal(localPath)},
                .remoteArchivePath = remote("/home/test/backup.tar"),
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }
        };

        const auto result = runToEnd(operation);
        ASSERT_FALSE(result.has_value());
        ASSERT_TRUE(serverStartResult->command("failWritesAfter", {{"bytes", nullptr}}));
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/backup.tar"), "old archive");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/backup.tar.filepart"), "<missing>");
    }

    TEST_F(TransferFailureTests, ArchiveUploadReplacesTheArchiveOnlyWhenComplete)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/backup.tar"}, {"content", "old archive"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        const auto localPath = localDirectory_.path() / "packed.txt";
        writeLocalFile(localPath, alphabet(4096));

        ArchiveUploadOperation refused{
            *sftp,
            {
                .localPaths = {toLocal(localPath)},
                .remoteArchivePath = remote("/home/test/backup.tar"),
                .compression = TarArchive::Compression::None,
            }
        };
        const auto refusedResult = runToEnd(refused);
        ASSERT_FALSE(refusedResult.has_value());
        EXPECT_EQ(refusedResult.error().type, SharedData::OperationErrorType::FileExists);
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/backup.tar"), "old archive");

        ArchiveUploadOperation replacing{
            *sftp,
            {
                .localPaths = {toLocal(localPath)},
                .remoteArchivePath = remote("/home/test/backup.tar"),
                .compression = TarArchive::Compression::None,
                .mayOverwrite = true,
            }
        };
        const auto replacingResult = runToEnd(replacing);
        ASSERT_TRUE(replacingResult.has_value()) << replacingResult.error().toString();
        EXPECT_NE(readRemoteFile(*sftp, "/home/test/backup.tar"), "old archive");
        EXPECT_EQ(readRemoteFile(*sftp, "/home/test/backup.tar.filepart"), "<missing>");
    }
}
