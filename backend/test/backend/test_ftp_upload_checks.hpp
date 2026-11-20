#pragma once

#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <constants/sftp.hpp>
#include <ftp/support/tree_responder.hpp>
#include <utility/temporary_directory.hpp>
#include "ftp_served_tree_tests.hpp"
#include "operation_runs.hpp"
#include "real_server_tests.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    /**
     * @brief Uploads to an FTP server that cannot look up a single path: its FEAT lists neither MLST nor SIZE.
     */
    class FtpWithoutProbesTests : public FtpServedTreeTests
    {
      protected:
        void serveWithoutProbes(std::vector<ServedEntry> work = {})
        {
            using FileTransferProtocol::Test::servedDirectory;
            serve({servedDirectory("work", std::move(work))}, ServedFeatures{.size = false});
        }

        /**
         * @brief An upload of the local file @p name, written with @p content, to /work/@p name.
         */
        UploadOperation::UploadOperationOptions uploadOf(std::string const& name, std::string_view content, bool fast)
        {
            const auto local = localDirectory_.path() / name;
            std::ofstream{local, std::ios::binary} << content;
            return {
                .remotePath = remote(fmt::format("/work/{}", name)),
                .localPath = toLocal(local),
                .unsafeFastMode = [fast]()
                {
                    return fast;
                },
            };
        }

        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
    };

    TEST_F(FtpWithoutProbesTests, TheFileSystemTellsItCannotProbe)
    {
        serveWithoutProbes();
        EXPECT_FALSE(fileSystem()->capabilities().probeEntries);
    }

    TEST_F(FtpWithoutProbesTests, AServerWithSizeCanProbe)
    {
        serve({});
        EXPECT_TRUE(fileSystem()->capabilities().probeEntries);
    }

    TEST_F(FtpWithoutProbesTests, ASafeUploadIsRefusedBeforeItTouchesTheServer)
    {
        serveWithoutProbes();
        UploadOperation operation{fileSystem(), uploadOf("safe.txt", "safe content", false)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_FALSE(tree_->exists("/work/safe.txt"));
        EXPECT_TRUE(tree_->stored().empty());
    }

    TEST_F(FtpWithoutProbesTests, AnUnsafeFastUploadReplacesAnExistingFile)
    {
        using FileTransferProtocol::Test::servedFile;
        serveWithoutProbes({servedFile("kept.txt", "old content")});
        UploadOperation operation{fileSystem(), uploadOf("kept.txt", "new content", true)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(tree_->contentOf("/work/kept.txt"), "new content");
    }

    TEST_F(FtpWithoutProbesTests, AnUnsafeFastUploadOfAnySizeGoesStraightToItsDestination)
    {
        serveWithoutProbes();
        const std::string content(Constants::unsafeFastSizeLimit + 1u, 'x');
        UploadOperation operation{fileSystem(), uploadOf("big.bin", content, true)};

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(tree_->contentOf("/work/big.bin"), content);
        EXPECT_EQ(tree_->stored(), (std::vector<std::string>{"/work/big.bin"}));
    }

    TEST_F(FtpWithoutProbesTests, AFolderUploadIsRefusedOnceAndCreatesNothing)
    {
        serveWithoutProbes();
        const auto local = localDirectory_.path() / "tree";
        std::filesystem::create_directories(local / "sub");
        for (auto const* name : {"a.txt", "b.txt", "sub/c.txt"})
            std::ofstream{local / name, std::ios::binary} << name;
        BulkUploadOperation operation{
            fileSystem(),
            {
                .remotePath = remote("/work/tree"),
                .localPath = toLocal(local),
                .individualOptions = {
                    .unsafeFastMode = []()
                    {
                        return false;
                    }
                },
            }
        };
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = toLocal(local)})
        );

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_FALSE(tree_->exists("/work/tree"));
        EXPECT_TRUE(tree_->stored().empty());
    }

    TEST_F(FtpWithoutProbesTests, AFolderUploadInTheUnsafeFastModeCreatesTheTree)
    {
        serveWithoutProbes();
        const auto local = localDirectory_.path() / "tree";
        std::filesystem::create_directories(local / "sub");
        for (auto const* name : {"a.txt", "sub/c.txt"})
            std::ofstream{local / name, std::ios::binary} << name;
        BulkUploadOperation operation{
            fileSystem(),
            {
                .remotePath = remote("/work/tree"),
                .localPath = toLocal(local),
                .individualOptions = {
                    .unsafeFastMode = []()
                    {
                        return true;
                    }
                },
            }
        };
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = toLocal(local)})
        );

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(tree_->contentOf("/work/tree/a.txt"), "a.txt");
        EXPECT_EQ(tree_->contentOf("/work/tree/sub/c.txt"), "sub/c.txt");
    }

    TEST_F(FtpWithoutProbesTests, ADownloadNeedsNoProbe)
    {
        using FileTransferProtocol::Test::servedFile;
        serveWithoutProbes({servedFile("file.txt", "content")});
        const auto local = localDirectory_.path() / "file.txt";
        DownloadOperation operation{
            fileSystem(), {.remotePath = remote("/work/file.txt"), .localPath = toLocal(local)}
        };

        const auto result = runToEnd(operation, operationLimit);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        std::ifstream file{local, std::ios::binary};
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}), "content");
    }
}
