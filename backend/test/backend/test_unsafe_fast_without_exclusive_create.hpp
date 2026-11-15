#pragma once

#include <backend/sftp/upload_operation.hpp>
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
     * @brief The unsafe fast mode on a file system that cannot create a file only where none exists, as FTP.
     */
    class UnsafeFastWithoutExclusiveCreateTests : public ::testing::Test
    {
      protected:
        UnsafeFastWithoutExclusiveCreateTests()
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }
        ~UnsafeFastWithoutExclusiveCreateTests() override
        {
            processingThread_.stop();
        }
        UnsafeFastWithoutExclusiveCreateTests(UnsafeFastWithoutExclusiveCreateTests const&) = delete;
        UnsafeFastWithoutExclusiveCreateTests& operator=(UnsafeFastWithoutExclusiveCreateTests const&) = delete;
        UnsafeFastWithoutExclusiveCreateTests(UnsafeFastWithoutExclusiveCreateTests&&) = delete;
        UnsafeFastWithoutExclusiveCreateTests& operator=(UnsafeFastWithoutExclusiveCreateTests&&) = delete;

        UploadOperation::UploadOperationOptions uploadOf(std::string const& name, std::string const& content) const
        {
            const auto local = localDirectory_.path() / name;
            std::ofstream{local, std::ios::binary} << content;
            return {
                .remotePath = remote("/home/test/" + name),
                .localPath = toLocal(local),
                .unsafeFastMode = []()
                {
                    return true;
                },
            };
        }

        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
        RemoteIo::ProcessingThread processingThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_{processingThread_.createStrand()};
        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> fake_{
            std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                strand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{}
            )
        };
    };

    TEST_F(UnsafeFastWithoutExclusiveCreateTests, AnUploadCreatesAMissingDestination)
    {
        fake_->addDirectory(remote("/home/test"));
        UploadOperation operation{fake_, uploadOf("new.txt", "new content")};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake_->contentOf(remote("/home/test/new.txt")), "new content");
        EXPECT_FALSE(fake_->exists(remote("/home/test/new.txt.filepart")));
        EXPECT_EQ(fake_->started<RemoteIo::Rename>(), 0u);
    }

    TEST_F(UnsafeFastWithoutExclusiveCreateTests, AnUploadLeavesAnExistingDestinationThatMayNotBeOverwritten)
    {
        fake_->addFile(remote("/home/test/kept.txt"), "kept content");
        UploadOperation operation{fake_, uploadOf("kept.txt", "new content")};

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().type, Operation::ErrorType::FileExists);
        EXPECT_EQ(fake_->contentOf(remote("/home/test/kept.txt")), "kept content");
        EXPECT_EQ(fake_->started<RemoteIo::OpenFile>(), 0u);
    }

    TEST_F(UnsafeFastWithoutExclusiveCreateTests, AnUploadOverwritesADestinationThatMayBeOverwrittenWithoutACheck)
    {
        fake_->addFile(remote("/home/test/replaced.txt"), "old content");
        auto options = uploadOf("replaced.txt", "new content");
        options.mayOverwrite = true;
        UploadOperation operation{fake_, std::move(options)};

        const auto result = runToEnd(operation);

        ASSERT_TRUE(result.has_value()) << result.error().toString();
        EXPECT_EQ(fake_->contentOf(remote("/home/test/replaced.txt")), "new content");
        EXPECT_EQ(fake_->started<RemoteIo::Lstat>(), 0u);
        EXPECT_EQ(fake_->started<RemoteIo::ProbeEntry>(), 0u);
    }

    TEST_F(UnsafeFastWithoutExclusiveCreateTests, AnUploadWhoseCheckFailsOpensNothing)
    {
        fake_->addDirectory(remote("/home/test"));
        fake_->failWhen<RemoteIo::ProbeEntry>(
            [](RemoteIo::ProbeEntry const&)
            {
                return true;
            },
            RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::ConnectionLost, .message = "The check was lost"}
        );
        UploadOperation operation{fake_, uploadOf("unchecked.txt", "new content")};

        const auto result = runToEnd(operation);

        ASSERT_FALSE(result.has_value());
        ASSERT_TRUE(result.error().remoteError.has_value());
        EXPECT_EQ(result.error().remoteError->kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(fake_->started<RemoteIo::OpenFile>(), 0u);
    }
}
