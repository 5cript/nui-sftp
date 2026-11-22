#pragma once

#include <backend/sftp/operation.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/fake_remote_file_system.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace Test
{
    namespace
    {
        struct FileSystemOpener : Operation
        {
            using Operation::openSession;
            using Operation::strandOf;
        };
    }

    class OperationBaseTests : public ::testing::Test
    {
      protected:
        OperationBaseTests()
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }
        ~OperationBaseTests() override
        {
            processingThread_.stop();
        }
        OperationBaseTests(OperationBaseTests const&) = delete;
        OperationBaseTests& operator=(OperationBaseTests const&) = delete;
        OperationBaseTests(OperationBaseTests&&) = delete;
        OperationBaseTests& operator=(OperationBaseTests&&) = delete;

        RemoteIo::ProcessingThread processingThread_{};
        std::unique_ptr<RemoteIo::ProcessingStrand> strand_{processingThread_.createStrand()};
        std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> fileSystem_{
            std::make_shared<RemoteIo::Test::FakeRemoteFileSystem>(
                strand_.get(),
                RemoteIo::Test::FakeRemoteFileSystem::Options{}
            )
        };
    };

    TEST_F(OperationBaseTests, OpensAnyRemoteFileSystemAsTheTypeItIsHeld)
    {
        const std::weak_ptr<RemoteIo::IRemoteFileSystem> asInterface = fileSystem_;
        const std::weak_ptr<RemoteIo::Test::FakeRemoteFileSystem> asItself = fileSystem_;

        const std::shared_ptr<RemoteIo::IRemoteFileSystem> openedAsInterface =
            FileSystemOpener::openSession(asInterface);
        const std::shared_ptr<RemoteIo::Test::FakeRemoteFileSystem> openedAsItself =
            FileSystemOpener::openSession(asItself);

        EXPECT_EQ(openedAsInterface.get(), fileSystem_.get());
        EXPECT_EQ(openedAsItself, fileSystem_);
        EXPECT_EQ(FileSystemOpener::strandOf(asInterface), strand_.get());
        EXPECT_EQ(FileSystemOpener::strandOf(asItself), strand_.get());
    }

    TEST_F(OperationBaseTests, OpensNothingOnceTheFileSystemIsGone)
    {
        const std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem = fileSystem_;
        fileSystem_.reset();

        EXPECT_EQ(FileSystemOpener::openSession(fileSystem), nullptr);
        EXPECT_EQ(FileSystemOpener::strandOf(fileSystem), nullptr);
    }

    TEST_F(OperationBaseTests, OpensNothingOnceItsStrandIsClosing)
    {
        const std::weak_ptr<RemoteIo::IRemoteFileSystem> fileSystem = fileSystem_;
        strand_->pushFinalTask([]() {});

        EXPECT_EQ(FileSystemOpener::openSession(fileSystem), nullptr);
        EXPECT_EQ(FileSystemOpener::strandOf(fileSystem), nullptr);
    }
}
