#pragma once

#include <backend/sftp/bulk_upload_operation.hpp>
#include <backend/sftp/local_scan_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <constants/sftp.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>
#include <utility/temporary_directory.hpp>
#include "operation_runs.hpp"
#include "real_server_tests.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    /**
     * @brief The checks of an upload: ProbeEntry where the file system answers it, and nothing but the unsafe fast mode
     *        where it cannot, as on an FTP server without MLST and SIZE.
     */
    class UploadCheckTests : public ::testing::Test
    {
      protected:
        UploadCheckTests()
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }
        ~UploadCheckTests() override
        {
            processingThread_.stop();
        }
        UploadCheckTests(UploadCheckTests const&) = delete;
        UploadCheckTests& operator=(UploadCheckTests const&) = delete;
        UploadCheckTests(UploadCheckTests&&) = delete;
        UploadCheckTests& operator=(UploadCheckTests&&) = delete;

        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> makeFake(bool probesEntries)
        {
            auto fake = std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                strand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{.capabilities = {.probeEntries = probesEntries}}
            );
            fake->addDirectory(remote("/home/test"));
            return fake;
        }

        Paths::LocalPath writeLocalFile(std::string const& name, std::string const& content) const
        {
            const auto local = localDirectory_.path() / name;
            std::filesystem::create_directories(local.parent_path());
            std::ofstream{local, std::ios::binary} << content;
            return toLocal(local);
        }

        UploadOperation::UploadOperationOptions
        uploadOf(std::string const& name, std::string const& content, bool unsafeFast) const
        {
            return {
                .remotePath = remote("/home/test/" + name),
                .localPath = writeLocalFile(name, content),
                .unsafeFastMode = [unsafeFast]()
                {
                    return unsafeFast;
                },
            };
        }

        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
        RemoteIo::ProcessingThread processingThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_{processingThread_.createStrand()};
    };

    TEST_F(UploadCheckTests, ASafeUploadChecksWithProbesAndListsNothing)
    {
        const auto fake = makeFake(true);
        UploadOperation operation{fake, uploadOf("safe.txt", "safe content", false)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/safe.txt")), "safe content");
        EXPECT_GT(fake->started<RemoteIo::ProbeEntry>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesASafeUploadIsRefusedBeforeItTouchesTheServer)
    {
        const auto fake = makeFake(false);
        UploadOperation operation{fake, uploadOf("safe.txt", "safe content", false)};

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_TRUE(result.error().extraInfo.has_value());
        EXPECT_EQ(fake->started<RemoteIo::OpenFile>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
        EXPECT_FALSE(fake->exists(remote("/home/test/safe.txt")));
    }

    TEST_F(UploadCheckTests, WithoutProbesAnUnsafeFastUploadReplacesAnExistingFile)
    {
        const auto fake = makeFake(false);
        fake->addFile(remote("/home/test/kept.txt"), "old content");
        UploadOperation operation{fake, uploadOf("kept.txt", "new content", true)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/kept.txt")), "new content");
        EXPECT_EQ(fake->started<RemoteIo::Lstat>(), 0u);
        EXPECT_EQ(fake->started<RemoteIo::ProbeEntry>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesAnUnsafeFastUploadOfAnySizeGoesStraightToItsDestination)
    {
        const auto fake = makeFake(false);
        const std::string content(Constants::unsafeFastSizeLimit, 'x');
        UploadOperation operation{fake, uploadOf("big.bin", content, true)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake->contentOf(remote("/home/test/big.bin")), content);
        EXPECT_FALSE(fake->exists(remote("/home/test/big.bin.filepart")));
        EXPECT_EQ(fake->started<RemoteIo::Rename>(), 0u);
    }

    TEST_F(UploadCheckTests, WithoutProbesAFolderUploadIsRefusedOnceAndCreatesNothing)
    {
        const auto fake = makeFake(false);
        for (auto const* name : {"tree/a.txt", "tree/b.txt", "tree/sub/c.txt"})
            writeLocalFile(name, name);
        const auto local = toLocal(localDirectory_.path() / "tree");
        BulkUploadOperation operation{
            fake,
            {
                .remotePath = remote("/home/test/tree"),
                .localPath = local,
                .individualOptions = {
                    .unsafeFastMode = []()
                    {
                        return false;
                    }
                },
            }
        };
        operation.scanFirst(
            std::make_unique<LocalScanOperation>(LocalScanOperation::ScanOperationOptions{.localPath = local})
        );

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::CannotCheckDestination);
        EXPECT_FALSE(fake->exists(remote("/home/test/tree")));
        EXPECT_EQ(fake->started<RemoteIo::OpenFile>(), 0u);
    }
}
