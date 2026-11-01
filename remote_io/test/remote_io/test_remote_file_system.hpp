#pragma once

#include <remote_io/async/buffer_provider.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/remote_file_system.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace RemoteIo::Test
{
    namespace RemoteFileSystemTestDetail
    {
        constexpr std::chrono::seconds replyTimeout{5};

        inline Paths::RemotePath remote(std::string_view path)
        {
            return Paths::RemotePath::make(path).value();
        }

        template <typename OperationT>
        OperationResult<OperationT> unsupported()
        {
            return std::unexpected(RemoteError{.kind = RemoteErrorKind::Unsupported});
        }

        /**
         * @brief Completes every operation when it starts, on the strand, like SFTP. Lstat answers with the size 42.
         */
        class ImmediateFileSystem final : public RemoteFileSystemImplementation<ImmediateFileSystem>
        {
          public:
            explicit ImmediateFileSystem(ProcessingStrand* strand)
                : strand_{strand}
            {}

            template <typename OperationT>
            void startOperation(Pending<OperationT> pending)
            {
                ++started;
                startedOnStrand = strand_->withinProcessingThread();
                if constexpr (std::is_same_v<OperationT, Lstat>)
                    pending.complete(FileInformation{.size = 42u});
                else
                    pending.complete(unsupported<OperationT>());
            }

            Capabilities capabilities() const override
            {
                return {};
            }
            ProcessingStrand* strand() const override
            {
                return strand_;
            }
            IBufferProvider& bufferProvider() override
            {
                return *bufferProvider_;
            }
            bool close() override
            {
                return false;
            }

            std::atomic_int started{0};
            std::atomic_bool startedOnStrand{false};

          protected:
            void dispatch(std::function<void()> start) override
            {
                strand_->pushTask(std::move(start));
            }
            void awaitInStrand(CompletionSignal&) override
            {
                ADD_FAILURE() << "an operation that completes when it starts is never awaited";
            }

          private:
            ProcessingStrand* strand_;
            std::shared_ptr<IBufferProvider> bufferProvider_{DefaultBufferProvider::create()};
        };

        /**
         * @brief Keeps every Lstat it starts until the test completes or drops it, like FTP; needs no strand to start
         *        one.
         */
        class DeferredFileSystem final : public RemoteFileSystemImplementation<DeferredFileSystem>
        {
          public:
            explicit DeferredFileSystem(ProcessingStrand* strand)
                : strand_{strand}
            {}

            template <typename OperationT>
            void startOperation(Pending<OperationT> pending)
            {
                if constexpr (std::is_same_v<OperationT, Lstat>)
                {
                    {
                        std::scoped_lock lock{mutex_};
                        if (gaveUp_)
                            return;
                        pending_.push_back(std::move(pending));
                    }
                    started_.notify_all();
                }
                else
                    pending.complete(unsupported<OperationT>());
            }

            /**
             * @brief Waits until an Lstat started and takes it. When none starts in time, later ones are dropped, so a
             *        failing test leaves no inStrand call waiting.
             */
            std::optional<Pending<Lstat>> takeStarted()
            {
                std::unique_lock lock{mutex_};
                if (!started_.wait_for(
                        lock,
                        replyTimeout,
                        [this]()
                        {
                            return !pending_.empty();
                        }
                    ))
                {
                    gaveUp_ = true;
                    return std::nullopt;
                }
                auto taken = std::move(pending_.front());
                pending_.pop_front();
                return taken;
            }

            Capabilities capabilities() const override
            {
                return {};
            }
            ProcessingStrand* strand() const override
            {
                return strand_;
            }
            IBufferProvider& bufferProvider() override
            {
                return *bufferProvider_;
            }
            bool close() override
            {
                return false;
            }

          protected:
            void dispatch(std::function<void()> start) override
            {
                start();
            }

          private:
            ProcessingStrand* strand_;
            std::shared_ptr<IBufferProvider> bufferProvider_{DefaultBufferProvider::create()};
            std::mutex mutex_{};
            std::condition_variable started_{};
            std::deque<Pending<Lstat>> pending_{};
            bool gaveUp_{false};
        };
    }

    using RemoteFileSystemTestDetail::DeferredFileSystem;
    using RemoteFileSystemTestDetail::ImmediateFileSystem;
    using RemoteFileSystemTestDetail::remote;
    using RemoteFileSystemTestDetail::replyTimeout;

    class RemoteFileSystemTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            processingThread_.start(std::chrono::milliseconds{1});
        }

        /**
         * @brief Stops the thread while the strand exists, so tasks a failed test left queued still run safely.
         */
        void TearDown() override
        {
            processingThread_.stop();
        }

        ProcessingThread processingThread_{};
        std::unique_ptr<ProcessingStrand> strand_{processingThread_.createStrand()};
    };

    TEST_F(RemoteFileSystemTests, OpenModesCombineAndAreFound)
    {
        const auto created = OpenMode::Write | OpenMode::Create | OpenMode::Truncate;

        EXPECT_TRUE(contains(created, OpenMode::Create));
        EXPECT_TRUE(contains(created, OpenMode::Write | OpenMode::Truncate));
        EXPECT_FALSE(contains(created, OpenMode::Exclusive));
        EXPECT_FALSE(contains(created, OpenMode::Read | OpenMode::Write));
    }

    TEST_F(RemoteFileSystemTests, CapabilitiesPromiseNothingByDefault)
    {
        const Capabilities capabilities{};

        EXPECT_FALSE(capabilities.symlinks);
        EXPECT_FALSE(capabilities.setModificationTime);
        EXPECT_FALSE(capabilities.exclusiveCreate);
        EXPECT_FALSE(capabilities.appendWrite);
        EXPECT_FALSE(capabilities.pipelinedTransfers);
        EXPECT_FALSE(capabilities.canonicalPath);
        EXPECT_FALSE(capabilities.userDirectories);
        EXPECT_EQ(capabilities.renameSemantics, RenameSemantics::Unknown);
        EXPECT_FALSE(capabilities.maxParallelTransfers.has_value());
        EXPECT_FALSE(capabilities.preferAsyncTransfers);
    }

    TEST_F(RemoteFileSystemTests, AnOperationKeepsItsFirstResult)
    {
        std::vector<std::uint64_t> sizes{};
        {
            Completion<Lstat> complete(
                [&sizes](OperationResult<Lstat> result)
                {
                    sizes.push_back(result.has_value() ? result->size : 0u);
                }
            );
            complete(FileInformation{.size = 3u});
            complete(FileInformation{.size = 4u});
        }

        EXPECT_EQ(sizes, std::vector<std::uint64_t>{3u});
    }

    TEST_F(RemoteFileSystemTests, AnOperationInStrandReturnsWhatTheFileSystemAnswered)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());

        auto answered = fileSystem->performPromise(
            [fileSystem]()
            {
                return fileSystem->inStrand(Lstat{.path = remote("/home/user/file.txt")});
            }
        );

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->size, 42u);
        EXPECT_EQ(fileSystem->started.load(), 1);
    }

    TEST_F(RemoteFileSystemTests, AnOperationStartedLaterRunsOnTheStrandOfASynchronousFileSystem)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());

        auto answered = fileSystem->later(Lstat{.path = remote("/home/user/file.txt")});

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->size, 42u);
        EXPECT_TRUE(fileSystem->startedOnStrand.load());
    }

    TEST_F(RemoteFileSystemTests, AnOperationThatCompletesAfterItStartedIsAwaitedInStrand)
    {
        auto fileSystem = std::make_shared<DeferredFileSystem>(strand_.get());

        auto answered = fileSystem->performPromise(
            [fileSystem]()
            {
                return fileSystem->inStrand(Lstat{.path = remote("/home/user/file.txt")});
            }
        );
        auto started = fileSystem->takeStarted();
        ASSERT_TRUE(started.has_value());
        started->complete(FileInformation{.size = 7u});

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->size, 7u);
    }

    TEST_F(RemoteFileSystemTests, AnOperationStartedLaterNeedsNoStrandWhereTheFileSystemStartsItAtOnce)
    {
        auto fileSystem = std::make_shared<DeferredFileSystem>(strand_.get());
        processingThread_.stop();

        auto answered = fileSystem->later(Lstat{.path = remote("/home/user/file.txt")});
        auto started = fileSystem->takeStarted();
        ASSERT_TRUE(started.has_value());
        started->complete(FileInformation{.size = 9u});

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_TRUE(information.has_value()) << information.error().toString();
        EXPECT_EQ(information->size, 9u);
    }

    TEST_F(RemoteFileSystemTests, AnInStrandWaitEndsWhenTheFileSystemDropsTheOperation)
    {
        auto fileSystem = std::make_shared<DeferredFileSystem>(strand_.get());

        auto answered = fileSystem->performPromise(
            [fileSystem]()
            {
                return fileSystem->inStrand(Lstat{.path = remote("/home/user/file.txt")});
            }
        );
        fileSystem->takeStarted().reset();

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_FALSE(information.has_value());
        EXPECT_EQ(information.error().wrapperError, WrapperErrors::TaskPushFailed);
    }

    TEST_F(RemoteFileSystemTests, AnOperationTheFileSystemDropsCompletesAsUnfinished)
    {
        auto fileSystem = std::make_shared<DeferredFileSystem>(strand_.get());

        auto answered = fileSystem->later(Lstat{.path = remote("/home/user/file.txt")});
        fileSystem->takeStarted().reset();

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_FALSE(information.has_value());
        EXPECT_EQ(information.error().wrapperError, WrapperErrors::TaskPushFailed);
    }

    TEST_F(RemoteFileSystemTests, AnOperationStartedLaterOnAStoppedStrandCompletesAsUnfinished)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());
        processingThread_.stop();

        auto answered = fileSystem->later(Lstat{.path = remote("/home/user/file.txt")});

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_FALSE(information.has_value());
        EXPECT_EQ(information.error().wrapperError, WrapperErrors::TaskPushFailed);
        EXPECT_EQ(fileSystem->started.load(), 0);
    }

    TEST_F(RemoteFileSystemTests, AnOperationStartedLaterAfterTheFileSystemIsGoneCompletesAsUnfinished)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());
        std::promise<void> release;
        fileSystem->perform(
            [released = release.get_future().share()]()
            {
                released.wait();
            }
        );

        auto answered = fileSystem->later(Lstat{.path = remote("/home/user/file.txt")});
        fileSystem.reset();
        release.set_value();

        ASSERT_EQ(answered.wait_for(replyTimeout), std::future_status::ready);
        const auto information = answered.get();
        ASSERT_FALSE(information.has_value());
        EXPECT_EQ(information.error().wrapperError, WrapperErrors::TaskPushFailed);
    }

    TEST_F(RemoteFileSystemTests, UnsetPathsNeverReachTheFileSystem)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());

        auto stat = fileSystem->later(Lstat{});
        auto rename = fileSystem->performPromise(
            [fileSystem]()
            {
                return fileSystem->inStrand(Rename{.source = remote("/home/user/a"), .destination = {}});
            }
        );
        auto removeAll = fileSystem->performPromise(
            [fileSystem]()
            {
                return fileSystem->inStrand(RemoveAll{.paths = {remote("/home/user/a"), Paths::RemotePath{}}});
            }
        );

        ASSERT_EQ(stat.wait_for(replyTimeout), std::future_status::ready);
        ASSERT_EQ(rename.wait_for(replyTimeout), std::future_status::ready);
        ASSERT_EQ(removeAll.wait_for(replyTimeout), std::future_status::ready);
        const auto statted = stat.get();
        const auto renamed = rename.get();
        const auto removed = removeAll.get();
        ASSERT_FALSE(statted.has_value());
        EXPECT_EQ(statted.error().wrapperError, WrapperErrors::InvalidPath);
        ASSERT_FALSE(renamed.has_value());
        EXPECT_EQ(renamed.error().wrapperError, WrapperErrors::InvalidPath);
        ASSERT_FALSE(removed.has_value());
        EXPECT_EQ(removed.error().wrapperError, WrapperErrors::InvalidPath);
        EXPECT_EQ(fileSystem->started.load(), 0);
    }

    TEST_F(RemoteFileSystemTests, TasksRunOnTheStrandOfTheFileSystem)
    {
        auto fileSystem = std::make_shared<ImmediateFileSystem>(strand_.get());

        const auto performed = std::make_shared<std::promise<bool>>();
        auto performedOnStrand = performed->get_future();
        fileSystem->perform(
            [performed, strand = strand_.get()]()
            {
                performed->set_value(strand->withinProcessingThread());
            }
        );
        auto promised = fileSystem->performPromise(
            [strand = strand_.get()]() -> std::expected<bool, RemoteError>
            {
                return strand->withinProcessingThread();
            }
        );

        ASSERT_EQ(performedOnStrand.wait_for(replyTimeout), std::future_status::ready);
        EXPECT_TRUE(performedOnStrand.get());
        ASSERT_EQ(promised.wait_for(replyTimeout), std::future_status::ready);
        const auto promisedOnStrand = promised.get();
        ASSERT_TRUE(promisedOnStrand.has_value());
        EXPECT_TRUE(*promisedOnStrand);
    }
}
