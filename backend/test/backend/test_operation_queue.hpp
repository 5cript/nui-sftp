#pragma once

#include <backend/local_delete_guard.hpp>
#include <backend/sftp/compared_state_check.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <backend/sftp/sequential_scan_progress.hpp>
#include "error_log_capture.hpp"
#include "nui_env.hpp"
#include "real_server_tests.hpp"

#include <log/log.hpp>
#include <nui/utility/scope_exit.hpp>
#include <paths/native.hpp>
#include <utility/permissions.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <variant>
#include <vector>

#ifndef _WIN32
#    include <unistd.h>
#endif

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class OperationQueueTests : public RealServerTests
    {
      public:
        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available; skipping OperationQueue tests";
            RealServerTests::SetUp();
        }

        void TearDown() override
        {
            if (queue_)
                runOnStrand([this]() {
                    queue_->dropAll();
                });
            queue_.reset();
            localQueue_.reset();
        }

        std::shared_ptr<OperationQueue> makeQueue(Persistence::SftpOptions options = {})
        {
            auto& env = NuiEnv::instance();
            queue_ = std::make_shared<OperationQueue>(
                queuePool_.get_executor(),
                strand_,
                *env.window,
                *env.hub,
                std::move(options),
                sessionId_,
                2
            );
            const auto observe = [this](SharedData::OperationCompleted const& completed)
            {
                std::scoped_lock lock{completionsMutex_};
                completions_.push_back(completed);
            };
            queue_->setCompletionObserver(observe);
            if (!localQueue_)
            {
                localQueue_ = std::make_shared<LocalOperationQueue>(
                    queuePool_.get_executor(),
                    strand_,
                    *env.window,
                    *env.hub,
                    std::make_shared<std::atomic_bool const>(false)
                );
                localQueue_->setCompletionObserver(observe);
            }
            return queue_;
        }

        /**
         * @brief Runs @p function on the queue's strand and waits for it, so it orders with the
         *        tasks the queue posts there itself.
         */
        template <typename FunctionT>
        auto runOnStrand(FunctionT&& function) -> std::invoke_result_t<FunctionT>
        {
            using ResultT = std::invoke_result_t<FunctionT>;
            auto promise = std::make_shared<std::promise<ResultT>>();
            auto future = promise->get_future();
            boost::asio::post(
                *strand_,
                [promise, function = std::forward<FunctionT>(function)]() mutable
                {
                    if constexpr (std::is_void_v<ResultT>)
                    {
                        function();
                        promise->set_value();
                    }
                    else
                        promise->set_value(function());
                }
            );
            return future.get();
        }

        /**
         * @brief Pauses the queue and holds its local deletes, as the session does.
         */
        void setPaused(bool pause)
        {
            queue_->paused(pause);
            runOnStrand([this, pause]() {
                localQueue_->hold(sessionId_, pause);
            });
        }

        /**
         * @brief Works the queue until @p expectedCompletions completions arrived.
         */
        void workUntilCompletions(std::size_t expectedCompletions)
        {
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                runOnStrand([this]() {
                    return queue_->work();
                });
                runOnStrand([]() {});
                {
                    std::scoped_lock lock{completionsMutex_};
                    if (completions_.size() >= expectedCompletions)
                        return;
                }
                std::this_thread::sleep_for(2ms);
            }
            ADD_FAILURE() << "Timed out waiting for " << expectedCompletions << " completions";
        }

        /**
         * @brief Works the queue like Session::doOperationQueueWork: again right away after progress, otherwise only
         *        once the queue woke. Fails when the queue stops calling for work before @p expectedCompletions arrived.
         */
        void workByWakesUntilCompletions(std::size_t expectedCompletions)
        {
            struct WakeState
            {
                std::mutex mutex{};
                std::condition_variable woken{};
                bool requested{false};
            };
            const auto state = std::make_shared<WakeState>();
            queue_->setWake(
                [state]()
                {
                    {
                        const std::scoped_lock lock{state->mutex};
                        state->requested = true;
                    }
                    state->woken.notify_one();
                }
            );
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                {
                    const std::scoped_lock lock{state->mutex};
                    state->requested = false;
                }
                const bool moreWork = runOnStrand([this]() {
                    return queue_->work();
                });
                runOnStrand([]() {});
                {
                    std::scoped_lock lock{completionsMutex_};
                    if (completions_.size() >= expectedCompletions)
                        return;
                }
                if (moreWork)
                    continue;
                std::unique_lock lock{state->mutex};
                if (!state->woken.wait_for(
                        lock,
                        5s,
                        [&state]
                        {
                            return state->requested;
                        }
                    ))
                {
                    ADD_FAILURE() << "The queue waited and was not woken";
                    return;
                }
            }
            ADD_FAILURE() << "Timed out waiting for " << expectedCompletions << " completions";
        }

        std::vector<SharedData::OperationCompleted> completions()
        {
            std::scoped_lock lock{completionsMutex_};
            return completions_;
        }

        /**
         * @brief What is at @p path on the server now, as a compare that ran right now would have seen it.
         */
        static SharedData::ComparedState remoteStateNow(SecureShell::SftpSession& sftp, std::string const& path)
        {
            auto future = sftp.performPromise(
                [&sftp, path = remote(path)]()
                {
                    return ComparedStateCheck::remoteStateOfInStrand(sftp, path);
                }
            );
            return future.get().value();
        }

        /**
         * @brief The resolved plan entry @p relKey of @p session, which the compare saw as what is at its target now.
         */
        static SyncSession::ResolvedPlanEntry plannedNow(
            SyncSession const& session,
            SecureShell::SftpSession& sftp,
            std::string relKey,
            SharedData::Sync::Action action,
            bool isDirectory,
            std::uint64_t size = 0
        )
        {
            using SharedData::Sync::Action;
            auto paths = session.resolve(Paths::RelativePath::make(relKey).value()).value();
            const bool onLocalSide = action == Action::Download || action == Action::DeleteLocal;
            auto compared = onLocalSide ? ComparedStateCheck::localStateOf(paths.local.native()).value()
                                        : remoteStateNow(sftp, paths.remote.value());
            return SyncSession::ResolvedPlanEntry{
                .entry = {.relKey = std::move(relKey), .action = action, .sizeBytes = size, .isDirectory = isDirectory},
                .paths = std::move(paths),
                .compared = std::move(compared),
            };
        }

        /**
         * @brief The permission bits of @p path on the server, unknown when it cannot be read.
         */
        static std::filesystem::perms remoteMode(SecureShell::SftpSession& sftp, std::string const& path)
        {
            auto future = sftp.stat(remote(path));
            if (future.wait_for(5s) != std::future_status::ready)
                return std::filesystem::perms::unknown;
            const auto information = future.get();
            return information ? information->permissions & std::filesystem::perms::mask
                               : std::filesystem::perms::unknown;
        }

        /**
         * @brief Options of a profile that turned inheriting permissions off for both directions.
         */
        static Persistence::SftpOptions notInheritingOptions()
        {
            Persistence::SftpOptions options{};
            options.downloadOptions.emplace().inheritPermissions = false;
            options.uploadOptions.emplace().inheritPermissions = false;
            return options;
        }

        static std::set<std::string> remoteNames(SecureShell::SftpSession& sftp, std::string const& directory)
        {
            auto future = sftp.listDirectory(remote(directory));
            if (future.wait_for(5s) != std::future_status::ready)
                return {"<timeout>"};
            auto result = future.get();
            if (!result.has_value())
                return {"<missing>"};
            std::set<std::string> names;
            for (auto const& entry : *result)
            {
                const auto name = entry.name;
                if (name != "." && name != "..")
                    names.insert(name);
            }
            return names;
        }

        /**
         * @brief Hands the queue's backups to @p registry and drops its operations, as a session teardown does.
         */
        void tearDownInto(BulkResumeRegistry& registry)
        {
            runOnStrand(
                [this, &registry]()
                {
                    queue_->dumpBulkResumes(registry);
                    queue_->dropAll();
                }
            );
            std::scoped_lock lock{completionsMutex_};
            completions_.clear();
        }

        /**
         * @brief A running replacement queue that adopted the backups of @p operationIds.
         */
        std::shared_ptr<OperationQueue> adoptInto(
            SecureShell::SftpSession& sftp,
            BulkResumeRegistry& registry,
            std::vector<Ids::OperationId> const& operationIds
        )
        {
            auto replacement = makeQueue();
            setPaused(false);
            runOnStrand(
                [&]()
                {
                    for (auto const& operationId : operationIds)
                    {
                        auto backup = registry.take(operationId);
                        EXPECT_TRUE(backup.has_value()) << operationId.value();
                        if (backup)
                            replacement->adoptBulkResume(sftp, operationId, *std::move(backup));
                    }
                }
            );
            return replacement;
        }

        /**
         * @brief The ids of all completions, after giving the queue a few more cycles to show extra ones.
         */
        std::multiset<std::string> completedIdsAfterSettling()
        {
            for (int cycle = 0; cycle < 50; ++cycle)
            {
                runOnStrand(
                    [this]()
                    {
                        return queue_->work();
                    }
                );
                std::this_thread::sleep_for(2ms);
            }
            std::multiset<std::string> ids;
            for (auto const& finished : completions())
            {
                EXPECT_EQ(finished.reason, SharedData::OperationCompletionReason::Completed);
                ids.insert(finished.operationId.value());
            }
            return ids;
        }

        static std::set<std::string> localNames(std::filesystem::path const& directory)
        {
            std::set<std::string> names;
            std::error_code errorCode{};
            for (auto const& entry : std::filesystem::directory_iterator{directory, errorCode})
                names.insert(Paths::nativeToUtf8(entry.path().filename()));
            return names;
        }

        /**
         * @brief Queues the delete of the file @p path as @p operationId, what its owner hears of its end goes to
         *        @p heard on the strand.
         */
        std::expected<void, Operation::Error> addOwnedDelete(
            SecureShell::SftpSession& sftp,
            Ids::OperationId const& operationId,
            std::string const& path,
            std::shared_ptr<std::vector<SharedData::OperationCompleted>> const& heard
        )
        {
            return runOnStrand(
                [&]()
                {
                    const auto added = queue_->addDeleteOperation(sftp, operationId, remote(path), false, false);
                    if (added)
                        queue_->whenCompleted(
                            operationId,
                            [heard](SharedData::OperationCompleted const& completed)
                            {
                                heard->push_back(completed);
                            }
                        );
                    return added;
                }
            );
        }

        /**
         * @brief What the observer of the progress of one operation heard, on the strand.
         */
        struct HeardProgress
        {
            std::vector<OperationQueue::OperationProgress> progress{};
            bool completed{false};
            std::size_t afterCompletion{0};
        };

        /**
         * @brief Observes the progress and the end of @p operationId. Call on the strand, before it starts.
         * @param onProgress Also hears each progress, after it was recorded.
         */
        std::shared_ptr<HeardProgress> observeProgress(
            Ids::OperationId const& operationId,
            std::function<void(OperationQueue::OperationProgress const&)> onProgress = {}
        )
        {
            auto heard = std::make_shared<HeardProgress>();
            queue_->whenProgressing(
                operationId,
                [heard, onProgress = std::move(onProgress)](OperationQueue::OperationProgress const& progress)
                {
                    if (heard->completed)
                        ++heard->afterCompletion;
                    heard->progress.push_back(progress);
                    if (onProgress)
                        onProgress(progress);
                }
            );
            queue_->whenCompleted(
                operationId,
                [heard](SharedData::OperationCompleted const&)
                {
                    heard->completed = true;
                }
            );
            return heard;
        }

        /**
         * @brief A copy of @p heard taken on the strand, after a few more cycles of the queue that let progress still
         *        on its way arrive.
         */
        HeardProgress heardAfterSettling(std::shared_ptr<HeardProgress> const& heard)
        {
            for (auto cycle = 0u; cycle < 5u; ++cycle)
            {
                runOnStrand(
                    [this]()
                    {
                        return queue_->work();
                    }
                );
                runOnStrand([]() {});
            }
            return runOnStrand(
                [&heard]()
                {
                    return *heard;
                }
            );
        }

        /**
         * @brief The progress in @p heard of the kind @p ProgressT, failing for any of another kind.
         */
        template <typename ProgressT>
        static std::vector<ProgressT> heardAs(HeardProgress const& heard)
        {
            std::vector<ProgressT> progress;
            for (auto const& each : heard.progress)
            {
                auto const* typed = std::get_if<ProgressT>(&each);
                EXPECT_NE(typed, nullptr) << "progress of another kind";
                if (typed)
                    progress.push_back(*typed);
            }
            return progress;
        }

        /**
         * @brief Expects @p progress of one file to rise from below @p totalBytes to it, reported for @p operationId.
         */
        static void expectRisingTo(
            std::vector<SharedData::TransferProgress> const& progress,
            Ids::OperationId const& operationId,
            std::uint64_t totalBytes
        )
        {
            ASSERT_GE(progress.size(), 2u);
            EXPECT_TRUE(std::ranges::is_sorted(progress, {}, &SharedData::TransferProgress::current));
            EXPECT_LT(progress.front().current, totalBytes);
            EXPECT_EQ(progress.back().current, totalBytes);
            for (auto const& each : progress)
            {
                EXPECT_EQ(each.operationId, operationId);
                EXPECT_EQ(each.max, totalBytes);
            }
        }

        /**
         * @brief Expects @p progress of a bulk to rise to @p entryCount entries of @p totalBytes, reported for
         *        @p operationId.
         */
        static void expectRisingTo(
            std::vector<SharedData::BulkProgress> const& progress,
            Ids::OperationId const& operationId,
            std::uint64_t entryCount,
            std::uint64_t totalBytes
        )
        {
            ASSERT_GE(progress.size(), 2u);
            EXPECT_TRUE(std::ranges::is_sorted(progress, {}, &SharedData::BulkProgress::bytesCurrent));
            EXPECT_TRUE(std::ranges::is_sorted(progress, {}, &SharedData::BulkProgress::fileCurrentIndex));
            EXPECT_LT(progress.front().bytesCurrent, totalBytes);
            EXPECT_EQ(progress.back().bytesCurrent, totalBytes);
            EXPECT_EQ(progress.back().fileCurrentIndex, entryCount);
            for (auto const& each : progress)
            {
                EXPECT_EQ(each.operationId, operationId);
                EXPECT_EQ(each.bytesTotal, totalBytes);
                EXPECT_EQ(each.fileCount, entryCount);
            }
        }

      protected:
        boost::asio::thread_pool queuePool_{2};
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_{
            std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(queuePool_.get_executor())};
        Ids::SessionId sessionId_{Ids::makeSessionId("operation-queue-test")};
        std::shared_ptr<OperationQueue> queue_{};
        /** @brief Runs the local deletes of sync plans, on the strand of the queue. */
        std::shared_ptr<LocalOperationQueue> localQueue_{};
        std::mutex completionsMutex_{};
        std::vector<SharedData::OperationCompleted> completions_{};
        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
    };

    TEST_F(OperationQueueTests, ALargeDownloadCompletesWithTheQueueDrivenOnlyByWakes)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        constexpr std::uint64_t size = 1024 * 1024;
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", size}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                Ids::generateOperationId(),
                toLocal(localDirectory_.path() / "big.bin"),
                remote("/home/test/big.bin"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        workByWakesUntilCompletions(1);

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(std::filesystem::file_size(localDirectory_.path() / "big.bin"), size);
    }

    TEST_F(OperationQueueTests, AddingAndResumingWakeTheQueue)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        const auto wakes = std::make_shared<std::atomic_int>(0);
        queue->setWake(
            [wakes]()
            {
                ++*wakes;
            }
        );

        setPaused(true);
        EXPECT_FALSE(runOnStrand([&]() {
            return queue->work();
        }));
        const auto paused = wakes->load();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                Ids::generateOperationId(),
                toLocal(localDirectory_.path() / "file1.txt"),
                remote("/home/test/file1.txt"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        EXPECT_GT(wakes->load(), paused);

        const auto added = wakes->load();
        setPaused(false);
        EXPECT_GT(wakes->load(), added);
        workByWakesUntilCompletions(1);
    }

    TEST_F(OperationQueueTests, CancellingADownloadThatWaitsForItsTransferEndsIt)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", 8 * 1024 * 1024}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                operationId,
                toLocal(localDirectory_.path() / "big.bin"),
                remote("/home/test/big.bin"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        // Worked until it waits for its transfer, nothing else drives it then.
        for (int step = 0; step < 100 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        runOnStrand([&]() {
            queue->cancel(operationId);
        });
        workUntilCompletions(1);

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
    }

    TEST_F(OperationQueueTests, ADownloadCanceledKeepingItsPartialFileLeavesItUntilItIsDiscarded)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", 8 * 1024 * 1024}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto operationId = Ids::generateOperationId();
        const auto partial = localDirectory_.path() / "big.bin.filepart";
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                operationId,
                toLocal(localDirectory_.path() / "big.bin"),
                remote("/home/test/big.bin"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        // Worked until it waits for its transfer, nothing else drives it then.
        for (int step = 0; step < 100 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(std::filesystem::exists(partial));

        runOnStrand([&]() {
            queue->cancel(operationId, OperationQueue::PartialFilesOnCancel::Kept);
        });
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_TRUE(completions().front().keptPartialFiles);
        EXPECT_TRUE(std::filesystem::exists(partial));

        runOnStrand([&]() {
            queue->discardKept(*sftp, operationId);
        });
        EXPECT_FALSE(std::filesystem::exists(partial));
    }

    TEST_F(OperationQueueTests, ACanceledDownloadDeletesItsPartialFileAsConfigured)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", 8 * 1024 * 1024}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto operationId = Ids::generateOperationId();
        const auto partial = localDirectory_.path() / "big.bin.filepart";
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                operationId,
                toLocal(localDirectory_.path() / "big.bin"),
                remote("/home/test/big.bin"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        // Worked until it waits for its transfer, nothing else drives it then.
        for (int step = 0; step < 100 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(std::filesystem::exists(partial));

        runOnStrand([&]() {
            queue->cancel(operationId);
        });
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_FALSE(completions().front().keptPartialFiles);
        EXPECT_FALSE(std::filesystem::exists(partial));
    }

    TEST_F(OperationQueueTests, ADownloadWithoutCleanupKeepsItsPartialFileOnAPlainCancel)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", 8 * 1024 * 1024}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        Persistence::SftpOptions options{};
        options.downloadOptions = Persistence::DownloadOptions{};
        options.downloadOptions->doCleanup = false;
        auto queue = makeQueue(std::move(options));
        setPaused(false);

        const auto operationId = Ids::generateOperationId();
        const auto partial = localDirectory_.path() / "big.bin.filepart";
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                operationId,
                toLocal(localDirectory_.path() / "big.bin"),
                remote("/home/test/big.bin"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        // Worked until it waits for its transfer, nothing else drives it then.
        for (int step = 0; step < 100 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(std::filesystem::exists(partial));

        runOnStrand([&]() {
            queue->cancel(operationId);
        });
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_TRUE(completions().front().keptPartialFiles);
        EXPECT_TRUE(std::filesystem::exists(partial));
    }

    TEST_F(OperationQueueTests, AnUploadCanceledKeepingItsPartialFileLeavesItOnTheServerUntilItIsDiscarded)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "big.bin";
        std::ofstream{localPath, std::ios::binary} << std::string(8 * 1024 * 1024, 'x');
        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp, operationId, toLocal(localPath), remote("/home/test/big.bin"), false, true, false, false
            );
        }).has_value());
        // Worked until it waits for its transfer, nothing else drives it then.
        for (int step = 0; step < 100 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(remoteNames(*sftp, "/home/test").contains("big.bin.filepart"));

        runOnStrand([&]() {
            queue->cancel(operationId, OperationQueue::PartialFilesOnCancel::Kept);
        });
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_TRUE(completions().front().keptPartialFiles);
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("big.bin.filepart"));

        runOnStrand([&]() {
            queue->discardKept(*sftp, operationId);
        });
        // Queued behind the removal on the processing thread.
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("big.bin.filepart"));
    }

    TEST_F(OperationQueueTests, ADirectoryDownloadCanceledKeepingItsPartialFilesContinuesWithoutWhatItFinished)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "Documents";
        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, operationId, toLocal(localPath), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());
        for (int step = 0; step < 100 && localNames(localPath).empty(); ++step)
            runOnStrand(
                [&]()
                {
                    return queue->work();
                }
            );
        ASSERT_EQ(localNames(localPath).size(), 1u);

        runOnStrand([&]() {
            queue->cancel(operationId, OperationQueue::PartialFilesOnCancel::Kept);
        });
        workUntilCompletions(1);
        ASSERT_TRUE(completions().front().keptPartialFiles);

        EXPECT_TRUE(runOnStrand([&]() {
            return queue->continueKept(*sftp, operationId);
        }));
        workUntilCompletions(2);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 2u);
        EXPECT_EQ(finished.back().operationId, operationId);
        EXPECT_EQ(finished.back().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.back().failedEntries.empty()) << finished.back().failedEntries.front().first;
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
        EXPECT_FALSE(runOnStrand([&]() {
            return queue->continueKept(*sftp, operationId);
        })) << "continued once only";
    }

    TEST_F(OperationQueueTests, DirectoryDownloadIsOneUnit)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto operationId = Ids::generateOperationId();
        const auto localPath = localDirectory_.path() / "Documents";
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, operationId, toLocal(localPath), remote("/home/test/Documents"), false, false, false, false
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, operationId);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, DirectoryUploadCompletionUsesFrontendId)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "toUpload";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "b.txt"} << "b";

        const auto operationId = Ids::generateOperationId();
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp, operationId, toLocal(localPath), remote("/home/test/uploaded"), false, false, false, false
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, operationId);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/uploaded"), (std::set<std::string>{"a.txt", "b.txt"}));
    }

    TEST_F(OperationQueueTests, UploadsWhoseSourceMayVanishEndCanceledWithoutATransferOnceItIsGone)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto file = localDirectory_.path() / "vanishing.txt";
        const auto directory = localDirectory_.path() / "vanishingDirectory";
        const auto kept = localDirectory_.path() / "mustExist.txt";
        std::ofstream{file} << "file";
        std::filesystem::create_directories(directory);
        std::ofstream{directory / "inside.txt"} << "inside";
        std::ofstream{kept} << "kept";
        const auto fileId = Ids::generateOperationId();
        const auto directoryId = Ids::generateOperationId();
        const auto keptId = Ids::generateOperationId();
        const auto add = [&](Ids::OperationId const& operationId, std::filesystem::path const& path, bool mayVanish)
        {
            return runOnStrand(
                [&]()
                {
                    return queue->addUploadOperation(
                        *sftp,
                        operationId,
                        toLocal(path),
                        remote("/home/test/" + path.filename().string()),
                        true,
                        false,
                        false,
                        false,
                        SharedData::OperationMode::Queued,
                        mayVanish
                    );
                }
            );
        };
        ASSERT_TRUE(add(fileId, file, true).has_value());
        ASSERT_TRUE(add(directoryId, directory, true).has_value());
        ASSERT_TRUE(add(keptId, kept, false).has_value());
        std::filesystem::remove(file);
        std::filesystem::remove_all(directory);
        std::filesystem::remove(kept);
        setPaused(false);

        workUntilCompletions(3);
        std::map<std::string, SharedData::OperationCompletionReason> reasons;
        for (auto const& completed : completions())
            reasons.emplace(completed.operationId.value(), completed.reason);
        EXPECT_EQ(reasons[fileId.value()], SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(reasons[directoryId.value()], SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(reasons[keptId.value()], SharedData::OperationCompletionReason::Failed);
        const auto names = remoteNames(*sftp, "/home/test");
        EXPECT_FALSE(names.contains("vanishing.txt"));
        EXPECT_FALSE(names.contains("vanishingDirectory"));
    }

    TEST_F(OperationQueueTests, AnUploadWhoseSourceMayVanishAndIsGoneAlreadyIsRefusedWithoutAnError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        const ErrorLogCapture errors;

        const auto add = [&](std::string const& name, bool mayVanish)
        {
            return runOnStrand(
                [&]()
                {
                    return queue->addUploadOperation(
                        *sftp,
                        Ids::generateOperationId(),
                        toLocal(localDirectory_.path() / name),
                        remote("/home/test/" + name),
                        true,
                        false,
                        false,
                        false,
                        SharedData::OperationMode::Queued,
                        mayVanish
                    );
                }
            );
        };
        const auto vanished = add("vanished.txt", true);
        const auto lost = add("lost.txt", false);

        ASSERT_FALSE(vanished.has_value());
        EXPECT_EQ(vanished.error().type, Operation::ErrorType::FileNotFound);
        ASSERT_FALSE(lost.has_value());
        EXPECT_EQ(lost.error().type, Operation::ErrorType::FileStatFailed);
        EXPECT_EQ(errors.mentioning("vanished.txt"), 0u);
        EXPECT_EQ(errors.mentioning("lost.txt"), 1u);
    }

    TEST_F(OperationQueueTests, ABulkUploadWhoseSourcesMayVanishSkipsAGoneFileWithoutAnError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        const ErrorLogCapture errors;

        const auto localPath = localDirectory_.path() / "gathered";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "kept.txt"} << "kept";
        std::ofstream{localPath / "vanished.txt"} << "vanished";
        const SharedData::BulkUploadRequest request{
            .entries =
                {
                    {.source = toLocal(localPath / "kept.txt"),
                        .destination = remote("/home/test/gathered/kept.txt"),
                        .sizeBytes = 4},
                    {.source = toLocal(localPath / "vanished.txt"),
                        .destination = remote("/home/test/gathered/vanished.txt"),
                        .sizeBytes = 8},
                },
            .allowOverwrite = true,
        };
        runOnStrand(
            [&]()
            {
                return queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    Ids::generateOperationId(),
                    /*sourceMayVanish=*/true
                );
            }
        );
        std::filesystem::remove(localPath / "vanished.txt");
        setPaused(false);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.front().failedEntries.empty());
        // The entry did not go, which its owner hears by index.
        ASSERT_EQ(finished.front().indexedFailures.size(), 1u);
        EXPECT_EQ(finished.front().indexedFailures.front().index, 1u);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/gathered"), std::set<std::string>{"kept.txt"});
        EXPECT_EQ(errors.mentioning("vanished.txt"), 0u);
    }

#ifndef _WIN32
    /**
     * @brief The messages at error level or above logged since the logger kept @p stashedBefore messages for the
     *        frontend.
     */
    inline std::vector<Log::Logger::StashedMessage> errorsStashedSince(std::size_t stashedBefore)
    {
        const auto stashed = Log::Detail::logger.stashed();
        std::vector<Log::Logger::StashedMessage> errors;
        std::ranges::copy_if(
            stashed | std::views::drop(stashedBefore),
            std::back_inserter(errors),
            [](Log::Logger::StashedMessage const& message)
            {
                return message.level >= Log::Level::Error;
            }
        );
        return errors;
    }

    TEST_F(OperationQueueTests, AnUploadWhoseOwnerListsItsFailuresLogsThemWithoutAToast)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can read any file";
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        const ErrorLogCapture errors;

        const auto file = localDirectory_.path() / "unreadable.txt";
        std::ofstream{file} << "unreadable";
        std::filesystem::permissions(file, std::filesystem::perms::none);
        const auto stashedBefore = Log::Detail::logger.stashed().size();
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(file),
                    remote("/home/test/unreadable.txt"),
                    true,
                    false,
                    false,
                    false,
                    SharedData::OperationMode::Queued,
                    false,
                    std::nullopt,
                    {},
                    std::nullopt,
                    Log::Toast::Skip
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();
        setPaused(false);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Failed);
        // Still logged, only without the toast.
        EXPECT_GT(errors.mentioning("unreadable.txt"), 0u);
        const auto logged = errorsStashedSince(stashedBefore);
        EXPECT_FALSE(logged.empty());
        for (auto const& message : logged)
            EXPECT_EQ(message.toast, Log::Toast::Skip) << message.message;
    }

    TEST_F(OperationQueueTests, ABulkUploadWhoseOwnerListsItsFailuresLogsThemWithoutAToast)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can read any file";
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        const ErrorLogCapture errors;

        const auto localPath = localDirectory_.path() / "partlyUnreadable";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "kept.txt"} << "kept";
        std::ofstream{localPath / "unreadable.txt"} << "unreadable";
        std::filesystem::permissions(localPath / "unreadable.txt", std::filesystem::perms::none);
        const SharedData::BulkUploadRequest request{
            .entries =
                {
                    {.source = toLocal(localPath / "kept.txt"),
                        .destination = remote("/home/test/partlyUnreadable/kept.txt"),
                        .sizeBytes = 4},
                    {.source = toLocal(localPath / "unreadable.txt"),
                        .destination = remote("/home/test/partlyUnreadable/unreadable.txt"),
                        .sizeBytes = 10},
                },
            .allowOverwrite = true,
        };
        const auto stashedBefore = Log::Detail::logger.stashed().size();
        runOnStrand(
            [&]()
            {
                return queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    Ids::generateOperationId(),
                    /*sourceMayVanish=*/true,
                    {},
                    Log::Toast::Skip
                );
            }
        );
        setPaused(false);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().failedEntries.size(), 1u);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/partlyUnreadable"), std::set<std::string>{"kept.txt"});
        EXPECT_GT(errors.mentioning("unreadable.txt"), 0u);
        const auto logged = errorsStashedSince(stashedBefore);
        EXPECT_FALSE(logged.empty());
        for (auto const& message : logged)
            EXPECT_EQ(message.toast, Log::Toast::Skip) << message.message;
    }

    TEST_F(OperationQueueTests, DirectoryUploadReportsAnUnreadableSubdirectory)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can list any directory";
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "withLocked";
        std::filesystem::create_directories(localPath / "locked");
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "locked" / "hidden.txt"} << "hidden";
        std::filesystem::permissions(localPath / "locked", std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[locked = localPath / "locked"]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp, operationId, toLocal(localPath), remote("/home/test/withLocked"), false, false, false, false
            );
        }).has_value());

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        ASSERT_EQ(finished.front().failedEntries.size(), 1u) << "an unreadable directory is never a silent success";
        EXPECT_EQ(finished.front().failedEntries.front().first, Paths::nativeToUtf8(localPath / "locked"));
        EXPECT_EQ(finished.front().failedEntries.front().second.type, SharedData::OperationErrorType::DirectoryUnreadable);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/withLocked"), (std::set<std::string>{"a.txt"}))
            << "the unreadable directory is not created empty";
    }

    TEST_F(OperationQueueTests, ArchiveUploadOfAnUnreadableSubdirectoryFails)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can list any directory";
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "archiveLocked";
        std::filesystem::create_directories(localPath / "locked");
        std::ofstream{localPath / "a.txt"} << "a";
        std::filesystem::permissions(localPath / "locked", std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[locked = localPath / "locked"]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(locked, std::filesystem::perms::owner_all, errorCode);
        }};

        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addArchiveUploadOperation(
                *sftp,
                operationId,
                {toLocal(localPath)},
                remote("/home/test/locked.tar"),
                TarArchive::Compression::None,
                {},
                false
            );
        }).has_value());

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Failed);
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("locked.tar"));
    }
#endif

    TEST_F(OperationQueueTests, CancellingARunningArchiveUploadEndsIt)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "archiveBig";
        std::filesystem::create_directories(localPath);
        {
            std::ofstream big{localPath / "big.bin", std::ios::binary};
            const std::string chunk(1024 * 1024, 'x');
            for (int index = 0; index < 32; ++index)
                big << chunk;
        }

        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addArchiveUploadOperation(
                *sftp,
                operationId,
                {toLocal(localPath)},
                remote("/home/test/big.tar.gz"),
                TarArchive::Compression::Gzip,
                {},
                false
            );
        }).has_value());
        for (int step = 0; step < 20 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(completions().empty());
        runOnStrand([&]() {
            queue->cancel(operationId);
        });
        workUntilCompletions(1);

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("big.tar.gz.filepart"));
    }

    TEST_F(OperationQueueTests, CancellingARunningArchiveDownloadEndsIt)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        constexpr std::uint64_t size = 32u * 1024u * 1024u;
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/big.bin"}, {"size", size}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto archivePath = localDirectory_.path() / "big.tar.gz";
        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addArchiveDownloadOperation(
                *sftp,
                operationId,
                {SharedData::ArchiveDownloadRoot{
                    .path = remote("/home/test/big.bin"),
                    .entry = SharedData::DirectoryEntry{
                        .name = "big.bin", .type = SharedData::FileType::Regular, .size = size
                    },
                }},
                toLocal(archivePath),
                TarArchive::Compression::Gzip,
                {},
                false
            );
        }).has_value());
        for (int step = 0; step < 20 && runOnStrand([&]() {
                 return queue->work();
             });
             ++step)
        {
        }
        ASSERT_TRUE(completions().empty());
        runOnStrand([&]() {
            queue->cancel(operationId);
        });
        workUntilCompletions(1);

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_FALSE(std::filesystem::exists(archivePath));
        EXPECT_FALSE(std::filesystem::exists(archivePath.string() + ".filepart"));
    }

    TEST_F(OperationQueueTests, DirectoryUploadCreatesDirectoriesWithTheDefaultModeWhenNotInheriting)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue(notInheritingOptions());
        setPaused(false);

        const auto localPath = localDirectory_.path() / "modes";
        std::filesystem::create_directories(localPath / "sub");
        std::ofstream{localPath / "sub" / "a.txt"} << "a";
        std::filesystem::permissions(localPath / "sub", std::filesystem::perms::all);

        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp, operationId, toLocal(localPath), remote("/home/test/modes"), false, false, false, false
            );
        }).has_value());
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);

        using std::filesystem::perms;
        // Owner everything, group read and enter, never world writable.
        const auto expected = perms::owner_all | perms::group_read | perms::group_exec;
        EXPECT_EQ(remoteMode(*sftp, "/home/test/modes"), expected);
        EXPECT_EQ(remoteMode(*sftp, "/home/test/modes/sub"), expected);
    }

#ifndef _WIN32
    TEST_F(OperationQueueTests, FilesKeepTheirModesThroughDownloadEditAndUploadWhenTheProfileDoesNotSetInheriting)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/plain.txt"}, {"content", "plain"}, {"mode", 0644}}
        ));
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/script.sh"}, {"content", "#!/bin/sh\n"}, {"mode", 0755}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        using std::filesystem::perms;
        const std::vector<std::pair<std::string, perms>> files{
            {"plain.txt", static_cast<perms>(0644u)},
            {"script.sh", static_cast<perms>(0755u)},
        };
        for (auto const& [name, mode] : files)
        {
            ASSERT_TRUE(runOnStrand([&]() {
                return queue->addDownloadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(localDirectory_.path() / name),
                    remote(fmt::format("/home/test/{}", name)),
                    false,
                    false,
                    false,
                    false
                );
            }).has_value());
        }
        workUntilCompletions(files.size());

        for (auto const& [name, mode] : files)
        {
            const auto localPath = localDirectory_.path() / name;
            EXPECT_EQ(std::filesystem::status(localPath).permissions(), mode & ~Utility::processUmask()) << name;
            std::ofstream{localPath, std::ios::app} << "edited\n";
            ASSERT_TRUE(runOnStrand([&]() {
                return queue->addUploadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(localPath),
                    remote(fmt::format("/home/test/{}", name)),
                    true,
                    false,
                    false,
                    false
                );
            }).has_value());
        }
        workUntilCompletions(files.size() * 2);

        for (auto const& finished : completions())
            EXPECT_EQ(finished.reason, SharedData::OperationCompletionReason::Completed);
        for (auto const& [name, mode] : files)
            EXPECT_EQ(remoteMode(*sftp, fmt::format("/home/test/{}", name)), mode & ~Utility::processUmask()) << name;
    }

    TEST_F(OperationQueueTests, DirectoryUploadKeepsTheModesOfItsEntriesWhenTheProfileDoesNotSetInheriting)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        using std::filesystem::perms;
        const auto localPath = localDirectory_.path() / "tree";
        std::filesystem::create_directories(localPath / "sub");
        std::ofstream{localPath / "sub" / "plain.txt"} << "plain";
        std::ofstream{localPath / "sub" / "script.sh"} << "#!/bin/sh\n";
        std::filesystem::permissions(localPath / "sub" / "plain.txt", static_cast<perms>(0644u));
        std::filesystem::permissions(localPath / "sub" / "script.sh", static_cast<perms>(0755u));
        std::filesystem::permissions(localPath / "sub", static_cast<perms>(0750u));

        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp,
                Ids::generateOperationId(),
                toLocal(localPath),
                remote("/home/test/tree"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);

        EXPECT_EQ(remoteMode(*sftp, "/home/test/tree/sub"), static_cast<perms>(0750u));
        EXPECT_EQ(remoteMode(*sftp, "/home/test/tree/sub/plain.txt"), static_cast<perms>(0644u));
        EXPECT_EQ(remoteMode(*sftp, "/home/test/tree/sub/script.sh"), static_cast<perms>(0755u));
    }

    TEST_F(OperationQueueTests, TransfersUseTheDefaultModesWhenTheProfileTurnsInheritingOff)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/script.sh"}, {"content", "#!/bin/sh\n"}, {"mode", 0755}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue(notInheritingOptions());
        setPaused(false);

        using std::filesystem::perms;
        const auto downloaded = localDirectory_.path() / "script.sh";
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDownloadOperation(
                *sftp,
                Ids::generateOperationId(),
                toLocal(downloaded),
                remote("/home/test/script.sh"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        workUntilCompletions(1);
        EXPECT_EQ(std::filesystem::status(downloaded).permissions(), Utility::newFilePermissions());

        const auto uploaded = localDirectory_.path() / "local.sh";
        std::ofstream{uploaded} << "#!/bin/sh\n";
        std::filesystem::permissions(uploaded, static_cast<perms>(0755u));
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp,
                Ids::generateOperationId(),
                toLocal(uploaded),
                remote("/home/test/local.sh"),
                false,
                false,
                false,
                false
            );
        }).has_value());
        workUntilCompletions(2);

        for (auto const& finished : completions())
            EXPECT_EQ(finished.reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(
            remoteMode(*sftp, "/home/test/local.sh"),
            perms::owner_read | perms::owner_write | perms::group_read | perms::group_write
        );
    }
#endif

    TEST_F(OperationQueueTests, BulkDownloadQueuesFilesAsOneCardAndDirectoriesUnderTheirIds)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto root = localDirectory_.path();
        const SharedData::BulkDownloadRequest request{
            .entries = {
                {.source = remote("/home/test/file1.txt"),
                    .destination = toLocal(root / "file1.txt"),
                    .sizeBytes = std::string{"Fake file content"}.size()},
                {.source = remote("/home/test/Pictures"),
                    .destination = toLocal(root / "Pictures"),
                    .isDirectory = true},
            },
        };
        const std::vector<Ids::OperationId> entryIds{Ids::generateOperationId(), Ids::generateOperationId()};
        const auto card = Ids::generateOperationId();
        EXPECT_EQ(
            runOnStrand(
                [&]()
                {
                    return queue->addBulkDownloadOperation(
                        *sftp,
                        request,
                        [&entryIds](std::size_t index)
                        {
                            return entryIds[index];
                        },
                        card
                    );
                }
            ),
            2u
        );

        workUntilCompletions(2);
        std::set<std::string> completed;
        for (auto const& finished : completions())
        {
            EXPECT_EQ(finished.reason, SharedData::OperationCompletionReason::Completed);
            completed.insert(finished.operationId.value());
        }
        EXPECT_EQ(completed, (std::set<std::string>{card.value(), entryIds[1].value()}));
        EXPECT_EQ(localNames(root), (std::set<std::string>{"Pictures", "file1.txt"}));
        EXPECT_EQ(localNames(root / "Pictures"), (std::set<std::string>{"image1.png", "image2.jpg"}));
    }

    TEST_F(OperationQueueTests, AdoptedBulkUploadIsQueuedAgainUnderItsCardId)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(true);

        const auto localPath = localDirectory_.path() / "toAdopt";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "b.txt"} << "b";
        const SharedData::BulkUploadRequest request{
            .entries = {
                {.source = toLocal(localPath / "a.txt"),
                    .destination = remote("/home/test/adopted_a.txt"),
                    .sizeBytes = 1},
                {.source = toLocal(localPath / "b.txt"),
                    .destination = remote("/home/test/adopted_b.txt"),
                    .sizeBytes = 1},
            },
        };
        const auto card = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                return queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    card
                );
            }
        );

        // The session goes away before the bulk ran, a replacement session adopts its backup.
        BulkResumeRegistry registry;
        runOnStrand(
            [&]()
            {
                queue->dumpBulkResumes(registry);
                queue->dropAll();
            }
        );
        auto backup = registry.take(card);
        ASSERT_TRUE(backup.has_value());
        ASSERT_TRUE(std::holds_alternative<SharedData::BulkUploadRequest>(backup->request));
        {
            std::scoped_lock lock{completionsMutex_};
            completions_.clear();
        }

        auto replacement = makeQueue();
        setPaused(false);
        runOnStrand(
            [&]()
            {
                replacement->adoptBulkResume(*sftp, card, *std::move(backup));
            }
        );

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, card);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        const auto names = remoteNames(*sftp, "/home/test");
        EXPECT_TRUE(names.contains("adopted_a.txt"));
        EXPECT_TRUE(names.contains("adopted_b.txt"));
    }

    TEST_F(OperationQueueTests, ADirectoryUploadWhoseBackupWasDiscardedIsNotResumed)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(true);

        const auto localPath = localDirectory_.path() / "watched";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        const auto kept = Ids::generateOperationId();
        const auto discarded = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                for (auto const& operationId : {kept, discarded})
                {
                    const auto added = queue->addUploadOperation(
                        *sftp,
                        operationId,
                        toLocal(localPath),
                        remote("/home/test/watched"),
                        /*allowOverwrite=*/true,
                        /*isBigFile=*/false,
                        /*insertRefresh=*/true,
                        /*createMissingDirectories=*/false,
                        SharedData::OperationMode::PriorityQueued,
                        /*sourceMayVanish=*/true
                    );
                    EXPECT_TRUE(added.has_value()) << added.error().toString();
                }
                queue->discardResumeBackup(discarded);
            }
        );

        BulkResumeRegistry registry;
        tearDownInto(registry);

        EXPECT_TRUE(registry.take(kept).has_value());
        EXPECT_FALSE(registry.take(discarded).has_value());
    }

    TEST_F(OperationQueueTests, AResumedBulkUploadLeavesOutTheFilesItFinished)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "partly";
        std::filesystem::create_directories(localPath);
        SharedData::BulkUploadRequest request{};
        for (auto const* name : {"partly_a.txt", "partly_b.txt", "partly_c.txt"})
        {
            std::ofstream{localPath / name} << name;
            request.entries.push_back(
                {.source = toLocal(localPath / name),
                    .destination = remote(fmt::format("/home/test/{}", name)),
                    .sizeBytes = std::string_view{name}.size()}
            );
        }
        const auto card = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                return queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    card
                );
            }
        );

        for (int step = 0; step < 50 && !remoteNames(*sftp, "/home/test").contains("partly_a.txt"); ++step)
            runOnStrand(
                [&]()
                {
                    return queue->work();
                }
            );
        ASSERT_TRUE(remoteNames(*sftp, "/home/test").contains("partly_a.txt"));
        ASSERT_FALSE(remoteNames(*sftp, "/home/test").contains("partly_c.txt")) << "the bulk ran to its end";

        BulkResumeRegistry registry;
        tearDownInto(registry);
        auto backup = registry.take(card);
        ASSERT_TRUE(backup.has_value());
        auto const& resumed = std::get<SharedData::BulkUploadRequest>(backup->request);
        ASSERT_FALSE(resumed.entries.empty());
        EXPECT_LT(resumed.entries.size(), 3u);
        for (auto const& entry : resumed.entries)
            EXPECT_NE(entry.destination.value(), "/home/test/partly_a.txt");
        EXPECT_EQ(backup->finished.entries, 3u - resumed.entries.size()) << "the card counts them as done";
        EXPECT_GE(backup->finished.bytes, std::string_view{"partly_a.txt"}.size());
        registry.store(card, *std::move(backup));

        adoptInto(*sftp, registry, {card});
        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, card);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.front().failedEntries.empty()) << finished.front().failedEntries.front().first;
        const auto names = remoteNames(*sftp, "/home/test");
        for (auto const* name : {"partly_a.txt", "partly_b.txt", "partly_c.txt"})
            EXPECT_TRUE(names.contains(name)) << name;
    }

    TEST_F(OperationQueueTests, AResumedSyncUploadKeepsWhatTheCompareSawAtItsDestinations)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "compared";
        std::filesystem::create_directories(localPath);
        SharedData::BulkUploadRequest request{};
        for (auto const* name : {"compared_a.txt", "compared_b.txt", "compared_c.txt"})
        {
            std::ofstream{localPath / name} << name;
            request.entries.push_back(
                {.source = toLocal(localPath / name),
                    .destination = remote(fmt::format("/home/test/{}", name)),
                    .sizeBytes = std::string_view{name}.size(),
                    .mayOverwrite = true,
                    .comparedDestination = SharedData::ComparedState{}}
            );
        }
        const auto card = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                return queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    card
                );
            }
        );
        for (int step = 0; step < 50 && !remoteNames(*sftp, "/home/test").contains("compared_a.txt"); ++step)
            runOnStrand(
                [&]()
                {
                    return queue->work();
                }
            );
        ASSERT_TRUE(remoteNames(*sftp, "/home/test").contains("compared_a.txt"));

        BulkResumeRegistry registry;
        tearDownInto(registry);
        auto backup = registry.take(card);
        ASSERT_TRUE(backup.has_value());
        auto const& resumed = std::get<SharedData::BulkUploadRequest>(backup->request);
        ASSERT_FALSE(resumed.entries.empty());
        for (auto const& entry : resumed.entries)
        {
            EXPECT_EQ(entry.comparedDestination, SharedData::ComparedState{}) << entry.destination.value();
            ASSERT_TRUE(serverStartResult->command(
                "createFile", {{"path", entry.destination.value()}, {"content", "appeared meanwhile"}}
            ));
        }
        const auto appeared = resumed.entries.size();
        registry.store(card, *std::move(backup));

        adoptInto(*sftp, registry, {card});
        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        ASSERT_EQ(finished.front().failedEntries.size(), appeared) << "the resumed entries replace nothing";
        for (auto const& [path, error] : finished.front().failedEntries)
            EXPECT_EQ(error.type, Operation::ErrorType::ChangedSinceCompare) << path;
    }

    TEST_F(OperationQueueTests, AResumedDirectoryDownloadLeavesOutTheFilesItFinished)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "Documents";
        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, operationId, toLocal(localPath), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());
        for (int step = 0; step < 100 && localNames(localPath).empty(); ++step)
            runOnStrand(
                [&]()
                {
                    return queue->work();
                }
            );
        ASSERT_EQ(localNames(localPath).size(), 1u);

        BulkResumeRegistry registry;
        tearDownInto(registry);
        auto backup = registry.take(operationId);
        ASSERT_TRUE(backup.has_value());
        auto const& resumed = std::get<SharedData::BulkDownloadRequest>(backup->request);
        ASSERT_EQ(resumed.entries.size(), 1u) << "only the file that is not there yet, as a flat list";
        EXPECT_FALSE(resumed.entries.front().isDirectory);
        EXPECT_FALSE(
            localNames(localPath).contains(Paths::nativeToUtf8(resumed.entries.front().destination.native().filename()))
        );
        ASSERT_TRUE(resumed.localRoot.has_value());
        EXPECT_EQ(resumed.localRoot->native(), localPath);
        EXPECT_EQ(backup->finished.entries, 1u);
        registry.store(operationId, *std::move(backup));

        adoptInto(*sftp, registry, {operationId});
        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, operationId);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.front().failedEntries.empty()) << finished.front().failedEntries.front().first;
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, ABackupOfSeveralDirectoriesIsRefused)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        makeQueue();
        setPaused(false);

        BulkResumeEntry backup{
            .request = SharedData::BulkDownloadRequest{
                .entries = {
                    {.source = remote("/home/test/Documents"),
                        .destination = toLocal(localDirectory_.path() / "Documents"),
                        .isDirectory = true},
                    {.source = remote("/home/test/Pictures"),
                        .destination = toLocal(localDirectory_.path() / "Pictures"),
                        .isDirectory = true},
                },
            },
        };
        const auto operationId = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                queue_->adoptBulkResume(*sftp, operationId, std::move(backup));
            }
        );
        EXPECT_TRUE(completedIdsAfterSettling().empty());
        EXPECT_TRUE(localNames(localDirectory_.path()).empty());
    }

    TEST_F(OperationQueueTests, QueuedRenameMovesTheFileButNeverReplacesTheDestination)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto moved = Ids::generateOperationId();
        const auto blocked = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addRenameOperation(
                    *sftp, moved, remote("/home/test/file1.txt"), remote("/home/test/moved.txt")
                );
            }
        ).has_value());
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addRenameOperation(
                    *sftp, blocked, remote("/home/test/Documents/doc2.txt"), remote("/home/test/Documents/doc1.txt")
                );
            }
        ).has_value());

        workUntilCompletions(2);
        std::map<std::string, SharedData::OperationCompletionReason> reasons;
        for (auto const& finished : completions())
            reasons[finished.operationId.value()] = finished.reason;
        EXPECT_EQ(reasons[moved.value()], SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(reasons[blocked.value()], SharedData::OperationCompletionReason::Failed);
        const auto names = remoteNames(*sftp, "/home/test");
        EXPECT_TRUE(names.contains("moved.txt"));
        EXPECT_FALSE(names.contains("file1.txt"));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Documents"), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, ACompletionCallbackRunsOnceForItsOperationOnly)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto moved = Ids::generateOperationId();
        const auto blocked = Ids::generateOperationId();
        std::vector<SharedData::OperationCompleted> called;
        runOnStrand(
            [&]()
            {
                ASSERT_TRUE(queue->addRenameOperation(
                    *sftp, moved, remote("/home/test/file1.txt"), remote("/home/test/moved.txt")
                ).has_value());
                ASSERT_TRUE(queue->addRenameOperation(
                    *sftp, blocked, remote("/home/test/Documents/doc2.txt"), remote("/home/test/Documents/doc1.txt")
                ).has_value());
                queue->whenCompleted(
                    blocked,
                    [&called](SharedData::OperationCompleted const& completed)
                    {
                        called.push_back(completed);
                    }
                );
            }
        );

        workUntilCompletions(2);
        runOnStrand([]() {});
        ASSERT_EQ(called.size(), 1u);
        EXPECT_EQ(called.front().operationId, blocked);
        EXPECT_EQ(called.front().reason, SharedData::OperationCompletionReason::Failed);
    }

    TEST_F(OperationQueueTests, ASingleUploadTellsItsObserverHowFarItGotUntilItsEnd)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        // Above the big-file cutoff, so it reports before its last byte went too.
        const auto localFile = localDirectory_.path() / "observed.bin";
        const auto size = 64u * 1024u;
        std::ofstream{localFile, std::ios::binary} << std::string(size, 'x');
        const auto upload = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp, upload, toLocal(localFile), remote("/home/test/observed.bin"), false, false, false, false
                );
            }
        ).has_value());
        const auto observed = runOnStrand(
            [&]()
            {
                return observeProgress(upload);
            }
        );

        workUntilCompletions(1);
        const auto heard = heardAfterSettling(observed);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(heard.completed);
        EXPECT_EQ(heard.afterCompletion, 0u);
        expectRisingTo(heardAs<SharedData::TransferProgress>(heard), upload, size);
    }

    TEST_F(OperationQueueTests, ASingleDownloadTellsItsObserverHowFarItGotUntilItsEnd)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        // The fixture above the big-file cutoff.
        const auto size = 65536u;
        const auto download = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp,
                    download,
                    toLocal(localDirectory_.path() / "large.txt"),
                    remote("/home/test/large.txt"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        const auto observed = runOnStrand(
            [&]()
            {
                return observeProgress(download);
            }
        );

        workUntilCompletions(1);
        const auto heard = heardAfterSettling(observed);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(heard.afterCompletion, 0u);
        expectRisingTo(heardAs<SharedData::TransferProgress>(heard), download, size);
    }

    TEST_F(OperationQueueTests, ADirectoryUploadTellsItsObserverHowFarItGotUntilItsEnd)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "observedDirectory";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "b.txt"} << "bb";
        const auto upload = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp,
                    upload,
                    toLocal(localPath),
                    remote("/home/test/observedDirectory"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        const auto observed = runOnStrand(
            [&]()
            {
                return observeProgress(upload);
            }
        );

        workUntilCompletions(1);
        const auto heard = heardAfterSettling(observed);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(heard.completed);
        EXPECT_EQ(heard.afterCompletion, 0u);
        expectRisingTo(heardAs<SharedData::BulkProgress>(heard), upload, 2u, 3u);
    }

    TEST_F(OperationQueueTests, ABulkUploadTellsTheObserverOfItsCardHowFarItGotUntilItsEnd)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "observedBulk";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "b.txt"} << "bb";
        const SharedData::BulkUploadRequest request{
            .entries = {
                {.source = toLocal(localPath / "a.txt"), .destination = remote("/home/test/bulkA.txt"), .sizeBytes = 1},
                {.source = toLocal(localPath / "b.txt"), .destination = remote("/home/test/bulkB.txt"), .sizeBytes = 2},
            },
        };
        const auto card = Ids::generateOperationId();
        const auto observed = runOnStrand(
            [&]()
            {
                queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    card
                );
                return observeProgress(card);
            }
        );

        workUntilCompletions(1);
        const auto heard = heardAfterSettling(observed);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(completions().front().failedEntries.empty());
        EXPECT_TRUE(heard.completed);
        EXPECT_EQ(heard.afterCompletion, 0u);
        expectRisingTo(heardAs<SharedData::BulkProgress>(heard), card, 2u, 3u);
    }

    TEST_F(OperationQueueTests, AnObserverStoppingItsUploadHearsNothingAfterTheEnd)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localFile = localDirectory_.path() / "stopped.bin";
        std::ofstream{localFile, std::ios::binary} << std::string(64u * 1024u, 'x');
        const auto upload = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp, upload, toLocal(localFile), remote("/home/test/stopped.bin"), false, false, false, false
                );
            }
        ).has_value());
        // Its end is handed on while the observer runs, which drops the observer from the queue.
        const auto observed = runOnStrand(
            [&]()
            {
                return observeProgress(
                    upload,
                    [weak = std::weak_ptr{queue}, upload](OperationQueue::OperationProgress const&)
                    {
                        if (const auto owner = weak.lock())
                            owner->cancel(upload);
                    }
                );
            }
        );

        workUntilCompletions(1);
        const auto heard = heardAfterSettling(observed);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_TRUE(heard.completed);
        EXPECT_EQ(heard.progress.size(), 1u);
        EXPECT_EQ(heard.afterCompletion, 0u);
    }

    TEST_F(OperationQueueTests, AnObserverHearsTheProgressOfItsOwnOperationOnly)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        // The queue has two slots, so both run in the same batches.
        const auto size = 64u * 1024u;
        const std::vector<std::string> names{"first.bin", "second.bin"};
        std::vector<Ids::OperationId> uploads;
        for (auto const& name : names)
        {
            std::ofstream{localDirectory_.path() / name, std::ios::binary} << std::string(size, 'x');
            uploads.push_back(Ids::generateOperationId());
            ASSERT_TRUE(runOnStrand(
                [&]()
                {
                    return queue->addUploadOperation(
                        *sftp,
                        uploads.back(),
                        toLocal(localDirectory_.path() / name),
                        remote(fmt::format("/home/test/{}", name)),
                        false,
                        false,
                        false,
                        false
                    );
                }
            ).has_value());
        }
        const auto stranger = Ids::generateOperationId();
        const auto [first, second, unrelated] = runOnStrand(
            [&]()
            {
                return std::tuple{observeProgress(uploads[0]), observeProgress(uploads[1]), observeProgress(stranger)};
            }
        );

        workUntilCompletions(2);
        expectRisingTo(heardAs<SharedData::TransferProgress>(heardAfterSettling(first)), uploads[0], size);
        expectRisingTo(heardAs<SharedData::TransferProgress>(heardAfterSettling(second)), uploads[1], size);
        const auto heardByStranger = heardAfterSettling(unrelated);
        EXPECT_TRUE(heardByStranger.progress.empty());
        EXPECT_FALSE(heardByStranger.completed);
    }

    TEST_F(OperationQueueTests, AdoptedDirectoryDownloadKeepsItsCardId)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        makeQueue();
        setPaused(true);

        const auto localPath = localDirectory_.path() / "Pictures";
        const auto operationId = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue_->addDownloadOperation(
                    *sftp, operationId, toLocal(localPath), remote("/home/test/Pictures"), false, false, false, false
                );
            }
        ).has_value());

        BulkResumeRegistry registry;
        tearDownInto(registry);
        adoptInto(*sftp, registry, {operationId});

        workUntilCompletions(1);
        EXPECT_EQ(completedIdsAfterSettling(), (std::multiset<std::string>{operationId.value()}));
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"image1.png", "image2.jpg"}));
    }

    TEST_F(OperationQueueTests, MixedBulkUploadResumesEachCardOnce)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        makeQueue();
        setPaused(true);

        const auto localPath = localDirectory_.path() / "mixed";
        std::filesystem::create_directories(localPath / "folder");
        std::ofstream{localPath / "loose.txt"} << "loose";
        std::ofstream{localPath / "folder" / "inner.txt"} << "inner";
        const SharedData::BulkUploadRequest request{
            .entries = {
                {.source = toLocal(localPath / "loose.txt"),
                    .destination = remote("/home/test/mixed_loose.txt"),
                    .sizeBytes = 5},
                {.source = toLocal(localPath / "folder"),
                    .destination = remote("/home/test/mixed_folder"),
                    .isDirectory = true},
            },
        };
        const std::vector<Ids::OperationId> entryIds{Ids::generateOperationId(), Ids::generateOperationId()};
        const auto card = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                return queue_->addBulkUploadOperation(
                    *sftp,
                    request,
                    [&entryIds](std::size_t index)
                    {
                        return entryIds[index];
                    },
                    card
                );
            }
        );

        // One backup per card: the files under the bulk card, the folder under its own id.
        BulkResumeRegistry registry;
        tearDownInto(registry);
        EXPECT_EQ(registry.size(), 2u);
        adoptInto(*sftp, registry, {card, entryIds[1]});

        workUntilCompletions(2);
        EXPECT_EQ(completedIdsAfterSettling(), (std::multiset<std::string>{card.value(), entryIds[1].value()}));
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("mixed_loose.txt"));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/mixed_folder"), (std::set<std::string>{"inner.txt"}));
    }

    TEST_F(OperationQueueTests, CancelledRecursiveDeleteDuringScanDeletesNothing)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/victim/keep.txt"}, {"content", "keep"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto directoryDelete = Ids::generateOperationId();
        const auto fileDelete = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, directoryDelete, remote("/home/test/victim"), true, false);
        }).has_value());
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, fileDelete, remote("/home/test/file2.txt"), false, false);
        }).has_value());

        // Starts the directory delete's scan, then cancels the unit while it scans.
        runOnStrand([&]() {
            return queue->work();
        });
        queue->cancel(directoryDelete);

        workUntilCompletions(2);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 2u);
        EXPECT_EQ(finished.front().operationId, directoryDelete);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(finished.back().operationId, fileDelete);
        EXPECT_EQ(finished.back().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/victim"), (std::set<std::string>{"keep.txt"}));
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("file2.txt"));
    }

    TEST_F(OperationQueueTests, BulkDeleteOfDirectoriesAndFilesIsOneUnit)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim"}}));
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim/inner"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/victim/inner/a.txt"}, {"content", "a"}}));
        ASSERT_TRUE(
            serverStartResult->command("createSymlink", {{"path", "/home/test/linkdir"}, {"target", "/home/test/Documents"}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto bulk = Ids::generateOperationId();
        const SharedData::BulkDeleteRequest request{
            .entries =
                {
                    {.target = remote("/home/test/victim"), .isDirectory = true},
                    {.target = remote("/home/test/linkdir"), .isDirectory = true},
                    {.target = remote("/home/test/file2.txt")},
                },
        };
        EXPECT_EQ(runOnStrand([&]() {
            return queue->addBulkDeleteOperation(*sftp, request, bulk);
        }), 3u);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, bulk);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        const auto home = remoteNames(*sftp, "/home/test");
        EXPECT_FALSE(home.contains("victim"));
        EXPECT_FALSE(home.contains("linkdir"));
        EXPECT_FALSE(home.contains("file2.txt"));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Documents"), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, BulkUploadReplacesOnlyApprovedDestinations)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "approval";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "a.txt"} << "a";
        std::ofstream{localPath / "b.txt"} << "b";
        // file1.txt appeared after the existence check, file2.txt existed and the user agreed to replace it.
        const SharedData::BulkUploadRequest request{
            .entries =
                {
                    {.source = toLocal(localPath / "a.txt"), .destination = remote("/home/test/file1.txt"), .sizeBytes = 1},
                    {.source = toLocal(localPath / "b.txt"),
                     .destination = remote("/home/test/file2.txt"),
                     .sizeBytes = 1,
                     .mayOverwrite = true},
                },
        };
        const auto card = Ids::generateOperationId();
        runOnStrand([&]() {
            return queue->addBulkUploadOperation(
                *sftp, request, [](std::size_t) { return Ids::generateOperationId(); }, card
            );
        });

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        ASSERT_EQ(finished.front().failedEntries.size(), 1u);
        EXPECT_EQ(finished.front().failedEntries.front().second.type, SharedData::OperationErrorType::FileExists);
        const auto sizeOf = [&](std::string const& path) -> std::uint64_t {
            auto future = sftp->stat(remote(path));
            if (future.wait_for(5s) != std::future_status::ready)
                return 0;
            const auto information = future.get();
            return information ? information->size : 0;
        };
        EXPECT_EQ(sizeOf("/home/test/file1.txt"), std::string{"Fake file content"}.size()) << "not approved, kept";
        EXPECT_EQ(sizeOf("/home/test/file2.txt"), 1u) << "approved, replaced";
    }

    TEST_F(OperationQueueTests, FailedUploadReportsItsError)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localFile = localDirectory_.path() / "refused.txt";
        std::ofstream{localFile} << "refused";
        const auto upload = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp, upload, toLocal(localFile), remote("/home/test/file1.txt"), false, false, false, false
            );
        }).has_value());

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Failed);
        ASSERT_TRUE(finished.front().error.has_value()) << "the card has to say why";
        EXPECT_EQ(finished.front().error->type, SharedData::OperationErrorType::FileExists);
    }

    TEST_F(OperationQueueTests, AChangedDestinationLeftToItsOwnerIsNoFailure)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localFile = localDirectory_.path() / "changed.txt";
        std::ofstream{localFile} << "changed";
        // Each expects nothing at file1.txt, which the server has.
        const auto add = [&](Ids::OperationId const& operationId, bool leftToOwner)
        {
            return runOnStrand(
                [&]()
                {
                    auto added = queue->addUploadOperation(
                        *sftp,
                        operationId,
                        toLocal(localFile),
                        remote("/home/test/file1.txt"),
                        true,
                        false,
                        false,
                        false,
                        SharedData::OperationMode::PriorityQueued,
                        false,
                        SharedData::ComparedState{}
                    );
                    if (added && leftToOwner)
                        queue->leaveChangedDestinationsToOwner(operationId);
                    return added;
                }
            );
        };
        const auto owned = Ids::generateOperationId();
        const auto unowned = Ids::generateOperationId();
        ASSERT_TRUE(add(owned, true).has_value());
        ASSERT_TRUE(add(unowned, false).has_value());
        const SharedData::BulkUploadRequest request{
            .entries =
                {
                    {.source = toLocal(localFile),
                        .destination = remote("/home/test/file2.txt"),
                        .sizeBytes = 7,
                        .mayOverwrite = true,
                        .comparedDestination = SharedData::ComparedState{}},
                    {.source = toLocal(localFile),
                        .destination = remote("/home/test/fresh.txt"),
                        .sizeBytes = 7,
                        .mayOverwrite = true,
                        .comparedDestination = SharedData::ComparedState{}},
                },
            .mode = SharedData::OperationMode::PriorityQueued,
        };
        const auto card = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    card
                );
                queue->leaveChangedDestinationsToOwner(card);
            }
        );

        workUntilCompletions(3);
        std::map<std::string, SharedData::OperationCompleted> finished;
        for (auto const& completed : completions())
            finished.emplace(completed.operationId.value(), completed);
        ASSERT_EQ(finished.size(), 3u);
        auto const& ownedEnd = finished.at(owned.value());
        EXPECT_EQ(ownedEnd.reason, SharedData::OperationCompletionReason::Canceled);
        ASSERT_TRUE(ownedEnd.error.has_value()) << "the owner has to learn why";
        EXPECT_EQ(ownedEnd.error->type, SharedData::OperationErrorType::ChangedSinceCompare);
        auto const& unownedEnd = finished.at(unowned.value());
        EXPECT_EQ(unownedEnd.reason, SharedData::OperationCompletionReason::Failed);
        ASSERT_TRUE(unownedEnd.error.has_value());
        EXPECT_EQ(unownedEnd.error->type, SharedData::OperationErrorType::ChangedSinceCompare);
        auto const& bulkEnd = finished.at(card.value());
        EXPECT_EQ(bulkEnd.reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(bulkEnd.failedEntries.empty());
        ASSERT_EQ(bulkEnd.indexedFailures.size(), 1u);
        EXPECT_EQ(bulkEnd.indexedFailures.front().index, 0u);
        EXPECT_EQ(bulkEnd.indexedFailures.front().type, SharedData::OperationErrorType::ChangedSinceCompare);
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("fresh.txt"));
    }

    TEST_F(OperationQueueTests, QueuedOperationsOfAClosedChannelFailWithChannelClosed)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto download = Ids::generateOperationId();
        const auto rename = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp,
                    download,
                    toLocal(localDirectory_.path() / "large.txt"),
                    remote("/home/test/large.txt"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addRenameOperation(
                    *sftp, rename, remote("/home/test/file1.txt"), remote("/home/test/moved.txt")
                );
            }
        ).has_value());

        sftp->close();
        sftp.reset();
        setPaused(false);
        workUntilCompletions(2);

        const auto finished = completions();
        ASSERT_EQ(finished.size(), 2u);
        for (auto const& completed : finished)
        {
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Failed) << completed.operationId.value();
            ASSERT_TRUE(completed.error.has_value()) << completed.operationId.value();
            EXPECT_EQ(completed.error->type, SharedData::OperationErrorType::ChannelClosed);
        }
        EXPECT_FALSE(std::filesystem::exists(localDirectory_.path() / "large.txt"));
    }

    TEST_F(OperationQueueTests, BulkDeleteGoesOnPastARootItCannotScan)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/doomed"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/doomed/a.txt"}, {"content", "a"}}));
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto bulk = Ids::generateOperationId();
        const SharedData::BulkDeleteRequest request{
            .entries = {
                {.target = remote("/home/test/Documents"), .isDirectory = true},
                {.target = remote("/home/test/doomed"), .isDirectory = true},
                {.target = remote("/home/test/file2.txt")},
            },
        };
        EXPECT_EQ(
            runOnStrand(
                [&]()
                {
                    return queue->addBulkDeleteOperation(*sftp, request, bulk);
                }
            ),
            3u
        );

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        ASSERT_EQ(finished.front().failedEntries.size(), 1u);
        EXPECT_EQ(finished.front().failedEntries.front().first, "/home/test/Documents");
        const auto home = remoteNames(*sftp, "/home/test");
        EXPECT_TRUE(home.contains("Documents"));
        EXPECT_FALSE(home.contains("doomed"));
        EXPECT_FALSE(home.contains("file2.txt"));
    }

    TEST(SequentialScanProgressTests, CountsOfLaterScansAddToTheFinishedOnes)
    {
        std::vector<std::array<std::uint64_t, 3>> reports;
        const auto next = ScanProgressDetail::sequentialScanProgress(
            [&reports](std::uint64_t bytes, std::uint64_t index, std::uint64_t scanned)
            {
                reports.push_back({bytes, index, scanned});
            }
        );
        auto first = next();
        auto second = next();
        first(10, 1, 1);
        first(30, 2, 2);
        second(5, 1, 1);
        second(7, 3, 3);
        EXPECT_EQ(reports.back(), (std::array<std::uint64_t, 3>{37, 5, 5}));
    }

    TEST_F(OperationQueueTests, BulkDeleteOfAnAlreadyDeletedItemSucceeds)
    {
        // A resumed bulk delete meets the items it deleted before the interruption.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto bulk = Ids::generateOperationId();
        const SharedData::BulkDeleteRequest request{
            .entries = {{.target = remote("/home/test/gone.txt")}, {.target = remote("/home/test/file2.txt")}},
        };
        EXPECT_EQ(runOnStrand([&]() { return queue->addBulkDeleteOperation(*sftp, request, bulk); }), 2u);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.front().failedEntries.empty());
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("file2.txt"));
    }

    TEST_F(OperationQueueTests, NonRecursiveDeleteKeepsANonEmptyDirectory)
    {
        // File tracking mirrors local deletions this way: a deleted local file never takes a remote directory along.
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, remove, remote("/home/test/Documents"), false, false);
        }).has_value());

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Failed);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Documents"), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, BulkDeleteOfListedContentKeepsUnlistedItems)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim"}}));
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim/inner"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/victim/listed.txt"}, {"content", "a"}}));
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/victim/inner/unlisted.txt"}, {"content", "b"}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto bulk = Ids::generateOperationId();
        const SharedData::BulkDeleteRequest request{
            .entries =
                {
                    {.target = remote("/home/test/victim"), .isDirectory = true},
                    {.target = remote("/home/test/victim/inner"), .isDirectory = true},
                    {.target = remote("/home/test/victim/listed.txt")},
                    {.target = remote("/home/test/file2.txt")},
                },
            .listedContentOnly = true,
        };
        EXPECT_EQ(runOnStrand([&]() {
            return queue->addBulkDeleteOperation(*sftp, request, bulk);
        }), 4u);

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        std::set<std::string> failed;
        for (auto const& [path, error] : finished.front().failedEntries)
            failed.insert(path);
        EXPECT_EQ(failed, (std::set<std::string>{"/home/test/victim", "/home/test/victim/inner"}));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/victim/inner"), (std::set<std::string>{"unlisted.txt"}));
        EXPECT_FALSE(remoteNames(*sftp, "/home/test/victim").contains("listed.txt"));
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("file2.txt")) << "a failure does not stop the rest";
    }

    TEST_F(OperationQueueTests, SyncPlanQueuesEveryPartUnderTheFrontendIds)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/syncRemote"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/syncRemote/down.txt"}, {"content", "down"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/syncRemote/gone.txt"}, {"content", "gone"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "syncExecution";
        std::filesystem::create_directories(localRoot);
        std::ofstream{localRoot / "up.txt"} << "up";
        std::ofstream{localRoot / "localGone.txt"} << "gone";

        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home/test/syncRemote"),
            },
            context.get_executor()
        };
        using SharedData::Sync::Action;
        const auto entry = [&session, &sftp](std::string relKey, Action action, bool isDirectory, std::uint64_t size = 0)
        {
            return plannedNow(session, *sftp, std::move(relKey), action, isDirectory, size);
        };
        SyncSession::ResolvedPlan plan{
            .entries =
                {
                    entry("up.txt", Action::Upload, false, 2),
                    entry("emptyA/emptyB", Action::Upload, true),
                    entry("down.txt", Action::Download, false, 4),
                    entry("newLocal/dir", Action::Download, true),
                    entry("gone.txt", Action::DeleteRemote, false),
                    entry("localGone.txt", Action::DeleteLocal, false),
                },
            .refused = {"CON"},
        };
        const SharedData::Sync::SyncExecutionRequest request{
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };

        const auto result = runOnStrand([&]() {
            return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
        });
        EXPECT_EQ(result.uploads, (std::vector<std::string>{"up.txt", "emptyA/emptyB"}));
        EXPECT_EQ(result.downloads, (std::vector<std::string>{"down.txt", "newLocal/dir"}));
        EXPECT_EQ(result.remoteDeletes, (std::vector<std::string>{"gone.txt"}));
        EXPECT_EQ(result.localDeletes, (std::vector<std::string>{"localGone.txt"}));
        EXPECT_EQ(result.remoteDeleteSources, (std::vector<std::string>{"/home/test/syncRemote/gone.txt"}));
        EXPECT_EQ(
            result.localDeleteSources,
            (std::vector<std::string>{Paths::LocalPath::make(localRoot / "localGone.txt").value().toUtf8()})
        );
        EXPECT_EQ(result.failed, (std::vector<std::string>{"CON"}));

        for (int cycle = 0; cycle < 10; ++cycle)
            runOnStrand([]() {});
        EXPECT_TRUE(std::filesystem::exists(localRoot / "localGone.txt")) << "the paused queue holds the local delete";
        setPaused(false);
        workUntilCompletions(4);
        std::set<std::string> finishedIds;
        for (auto const& completed : completions())
        {
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed) << completed.operationId.value();
            finishedIds.insert(completed.operationId.value());
        }
        EXPECT_EQ(
            finishedIds,
            (std::set<std::string>{
                request.uploadId.value(),
                request.downloadId.value(),
                request.remoteDeleteId.value(),
                request.localDeleteId.value(),
            })
        );
        EXPECT_EQ(remoteNames(*sftp, "/home/test/syncRemote"), (std::set<std::string>{"down.txt", "emptyA", "up.txt"}));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/syncRemote/emptyA"), (std::set<std::string>{"emptyB"}));
        EXPECT_TRUE(std::filesystem::exists(localRoot / "down.txt"));
        EXPECT_TRUE(std::filesystem::is_directory(localRoot / "newLocal" / "dir"));
        EXPECT_FALSE(std::filesystem::exists(localRoot / "localGone.txt"));
    }

    TEST_F(OperationQueueTests, SyncTransfersFilesOfSiblingDirectoriesBelowTheRoots)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/siblings"}}));
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/siblings/src"}}));
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/siblings/src/a.cpp"}, {"content", "a"}})
        );
        ASSERT_TRUE(
            serverStartResult->command("createFile", {{"path", "/home/test/siblings/README.md"}, {"content", "readme"}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "siblings";
        std::filesystem::create_directories(localRoot / "lib");
        std::ofstream{localRoot / "lib" / "b.cpp"} << "b";
        std::ofstream{localRoot / "NOTES.md"} << "notes";

        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home/test/siblings"),
            },
            context.get_executor()
        };
        using SharedData::Sync::Action;
        const auto entry = [&session](std::string relKey, Action action, std::uint64_t size)
        {
            auto paths = session.resolve(Paths::RelativePath::make(relKey).value()).value();
            return SyncSession::ResolvedPlanEntry{
                .entry = {.relKey = std::move(relKey), .action = action, .sizeBytes = size},
                .paths = std::move(paths),
            };
        };
        // The first file of each direction lies in a subdirectory, its parent is not the root of the others.
        SyncSession::ResolvedPlan plan{
            .entries = {
                entry("src/a.cpp", Action::Download, 1),
                entry("README.md", Action::Download, 6),
                entry("lib/b.cpp", Action::Upload, 1),
                entry("NOTES.md", Action::Upload, 5),
            },
        };
        const SharedData::Sync::SyncExecutionRequest request{
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };
        runOnStrand(
            [&]()
            {
                return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
            }
        );

        setPaused(false);
        workUntilCompletions(2);
        for (auto const& completed : completions())
        {
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed)
                << completed.operationId.value();
            EXPECT_TRUE(completed.failedEntries.empty()) << completed.operationId.value();
        }
        EXPECT_TRUE(std::filesystem::exists(localRoot / "src" / "a.cpp"));
        EXPECT_TRUE(std::filesystem::exists(localRoot / "README.md"));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/siblings/lib"), (std::set<std::string>{"b.cpp"}));
        EXPECT_TRUE(remoteNames(*sftp, "/home/test/siblings").contains("NOTES.md"));
    }

#ifndef _WIN32
    TEST_F(OperationQueueTests, SyncCreatesEmptyDirectoriesInItsBulksButNeverThroughALink)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/emptyDirs"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "emptyDirs";
        const auto outside = localDirectory_.path() / "emptyDirsOutside";
        std::filesystem::create_directories(localRoot);
        std::filesystem::create_directories(outside);
        std::filesystem::create_directory_symlink(outside, localRoot / "linked");

        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home/test/emptyDirs"),
            },
            context.get_executor()
        };
        using SharedData::Sync::Action;
        const auto entry = [&session](std::string relKey, Action action)
        {
            auto paths = session.resolve(Paths::RelativePath::make(relKey).value()).value();
            return SyncSession::ResolvedPlanEntry{
                .entry = {.relKey = std::move(relKey), .action = action, .isDirectory = true},
                .paths = std::move(paths),
            };
        };
        SyncSession::ResolvedPlan plan{
            .entries = {
                entry("linked/inside", Action::Download),
                entry("fresh/deep", Action::Download),
                entry("up/deep", Action::Upload),
            },
        };
        const SharedData::Sync::SyncExecutionRequest request{
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };
        const auto result = runOnStrand(
            [&]()
            {
                return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
            }
        );
        EXPECT_EQ(result.downloads, (std::vector<std::string>{"linked/inside", "fresh/deep"}));
        EXPECT_EQ(result.uploads, (std::vector<std::string>{"up/deep"}));

        setPaused(false);
        workUntilCompletions(2);
        for (auto const& completed : completions())
        {
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed)
                << completed.operationId.value();
            if (completed.operationId == request.downloadId)
                EXPECT_EQ(completed.failedEntries.size(), 1u);
            else
                EXPECT_TRUE(completed.failedEntries.empty());
        }
        EXPECT_FALSE(std::filesystem::exists(outside / "inside"));
        EXPECT_TRUE(std::filesystem::is_directory(localRoot / "fresh" / "deep"));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/emptyDirs/up"), (std::set<std::string>{"deep"}));
    }
#endif

    TEST_F(OperationQueueTests, SyncLeavesWhatChangedBetweenTheCompareAndItsExecution)
    {
        using namespace SharedData::Sync;
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/changing"}}));
        for (auto const& [name, content] :
             {std::pair{"edited.txt", "old"}, std::pair{"gone.txt", "gone"}, std::pair{"kept.txt", "kept"}})
        {
            ASSERT_TRUE(serverStartResult->command(
                "createFile", {{"path", fmt::format("/home/test/changing/{}", name)}, {"content", content}}
            ));
        }
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "changing";
        std::filesystem::create_directories(localRoot);
        std::ofstream{localRoot / "edited.txt"} << "new local content";
        std::ofstream{localRoot / "up.txt"} << "up";
        std::ofstream{localRoot / "appears.txt"} << "local";

        // The compare sees both sides as they are now.
        const auto nodeOf = [](std::string name, SharedData::ComparedState const& state)
        {
            return ScanNode{.name = std::move(name), .type = state.type, .size = state.size, .mtime = state.mtime};
        };
        const auto treeOf = [&](std::vector<std::string> const& names, auto const& stateOf)
        {
            std::vector<ScanNode> children;
            for (auto const& name : names)
                children.push_back(nodeOf(name, stateOf(name)));
            std::ranges::sort(children, {}, &ScanNode::name);
            return ScanNode{.name = "", .type = SharedData::FileType::Directory, .children = std::move(children)};
        };
        const auto localTree = [&]()
        {
            return treeOf(
                {"appears.txt", "edited.txt", "up.txt"},
                [&](std::string const& name)
                {
                    return ComparedStateCheck::localStateOf(localRoot / name).value();
                }
            );
        };
        const auto remoteTree = [&](std::vector<std::string> const& names)
        {
            return treeOf(
                names,
                [&](std::string const& name)
                {
                    return remoteStateNow(*sftp, fmt::format("/home/test/changing/{}", name));
                }
            );
        };

        boost::asio::io_context context{};
        SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home/test/changing"),
            },
            context.get_executor()
        };
        const DiffOptions options{.direction = Direction::Upload, .actionDelete = true};
        session.setLocalTreeInStrand(localTree());
        session.setRemoteTreeInStrand(remoteTree({"edited.txt", "gone.txt", "kept.txt"}));
        const auto summary = session.recomputeDiffInStrand(options, {});
        EXPECT_EQ(summary.uploads.itemCount, 3u);
        EXPECT_EQ(summary.deletes.itemCount, 2u);

        const auto planOf = [&](DiffSection section, std::unordered_set<std::string> const& selected)
        {
            return session.resolvePlanInStrand(section, selected);
        };
        auto uploadPlan = planOf(DiffSection::Upload, {"appears.txt", "edited.txt", "up.txt"});
        auto deletePlan = planOf(DiffSection::Delete, {"gone.txt", "kept.txt"});

        // Meanwhile on the server: one destination is edited, one appears and one delete target changes.
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/changing/edited.txt"}, {"content", "edited on the server"}}
        ));
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/changing/appears.txt"}, {"content", "remote"}}
        ));
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/changing/kept.txt"}, {"content", "kept and edited"}}
        ));

        const auto execute = [&](DiffSection section, SyncSession::ResolvedPlan plan)
        {
            const SyncExecutionRequest request{
                .section = section,
                .uploadId = Ids::generateOperationId(),
                .downloadId = Ids::generateOperationId(),
                .remoteDeleteId = Ids::generateOperationId(),
                .localDeleteId = Ids::generateOperationId(),
            };
            return runOnStrand(
                [&]()
                {
                    return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
                }
            );
        };
        const auto uploads = execute(DiffSection::Upload, std::move(uploadPlan));
        EXPECT_EQ(uploads.uploads.size(), 3u);
        const auto deletes = execute(DiffSection::Delete, std::move(deletePlan));
        EXPECT_EQ(deletes.remoteDeletes.size(), 2u);

        setPaused(false);
        workUntilCompletions(2);
        std::map<std::string, Operation::ErrorType> failures;
        for (auto const& completed : completions())
        {
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed)
                << completed.operationId.value();
            for (auto const& [path, error] : completed.failedEntries)
                failures.emplace(path.substr(path.rfind('/') + 1), error.type);
        }
        EXPECT_EQ(
            failures,
            (std::map<std::string, Operation::ErrorType>{
                {"appears.txt", Operation::ErrorType::FileExists},
                {"edited.txt", Operation::ErrorType::ChangedSinceCompare},
                {"kept.txt", Operation::ErrorType::ChangedSinceCompare},
            })
        );
        EXPECT_EQ(
            remoteNames(*sftp, "/home/test/changing"),
            (std::set<std::string>{"appears.txt", "edited.txt", "kept.txt", "up.txt"})
        );
        EXPECT_EQ(remoteStateNow(*sftp, "/home/test/changing/edited.txt").size, std::string{"edited on the server"}.size());
        EXPECT_EQ(remoteStateNow(*sftp, "/home/test/changing/appears.txt").size, std::string{"remote"}.size());

        // A recompare offers the entries that were left alone again.
        session.setRemoteTreeInStrand(remoteTree({"appears.txt", "edited.txt", "kept.txt", "up.txt"}));
        const auto again = session.recomputeDiffInStrand(options, {});
        EXPECT_EQ(again.uploads.itemCount, 2u);
        EXPECT_EQ(again.deletes.itemCount, 1u);
    }

    TEST_F(OperationQueueTests, SyncTransfersNonAsciiNames)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/Größe"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/Größe/東京.txt"}, {"content", "down"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / Paths::nativeFromUtf8("Jürgen");
        std::filesystem::create_directories(localRoot / Paths::nativeFromUtf8("Ärger"));
        std::ofstream{localRoot / Paths::nativeFromUtf8("Ärger") / Paths::nativeFromUtf8("Ölung.txt")} << "up";

        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home/test/Größe"),
            },
            context.get_executor()
        };
        using SharedData::Sync::Action;
        const auto entry = [&session](std::string relKey, Action action, std::uint64_t size)
        {
            auto paths = session.resolve(Paths::RelativePath::make(relKey).value()).value();
            return SyncSession::ResolvedPlanEntry{
                .entry = {.relKey = std::move(relKey), .action = action, .sizeBytes = size, .isDirectory = false},
                .paths = std::move(paths),
            };
        };
        SyncSession::ResolvedPlan plan{
            .entries =
                {
                    entry("Ärger/Ölung.txt", Action::Upload, 2),
                    entry("東京.txt", Action::Download, 4),
                },
        };
        const SharedData::Sync::SyncExecutionRequest request{
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };

        const auto result = runOnStrand([&]() {
            return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
        });
        EXPECT_EQ(result.uploads, (std::vector<std::string>{"Ärger/Ölung.txt"}));
        EXPECT_EQ(result.downloads, (std::vector<std::string>{"東京.txt"}));

        setPaused(false);
        workUntilCompletions(2);
        for (auto const& completed : completions())
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed) << completed.operationId.value();
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Größe/Ärger"), (std::set<std::string>{"Ölung.txt"}));
        EXPECT_EQ(localNames(localRoot), (std::set<std::string>{"Ärger", "東京.txt"}));
    }

#ifndef _WIN32
    TEST_F(OperationQueueTests, SyncRefusesEveryDeleteBelowAProtectedDirectory)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/other.txt"}, {"content", "other"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "syncProtected";
        std::filesystem::create_directories(localRoot / "home" / "inner");
        std::ofstream{localRoot / "home" / "keep.txt"} << "keep";
        std::ofstream{localRoot / "home" / "inner" / "keep.txt"} << "keep";
        std::ofstream{localRoot / "loose.txt"} << "loose";

        auto const* previousHome = std::getenv("HOME");
        const std::optional<std::string> savedHome =
            previousHome ? std::optional<std::string>{previousHome} : std::nullopt;
        ASSERT_EQ(setenv("HOME", (localRoot / "home").c_str(), 1), 0);
        const auto restoreHome = Nui::ScopeExit{[&savedHome]() noexcept
            {
                if (savedHome)
                    setenv("HOME", savedHome->c_str(), 1);
                else
                    unsetenv("HOME");
            }};

        boost::asio::io_context context{};
        const SyncSession session{
            SyncSession::Options{
                .localRoot = Paths::LocalPath::make(localRoot).value(),
                .remoteRoot = remote("/home"),
            },
            context.get_executor()
        };
        using SharedData::Sync::Action;
        const auto entry = [&session, &sftp](std::string relKey, Action action, bool isDirectory)
        {
            return plannedNow(session, *sftp, std::move(relKey), action, isDirectory);
        };
        SyncSession::ResolvedPlan plan{
            .entries =
                {
                    entry("test", Action::DeleteRemote, true),
                    entry("test/file2.txt", Action::DeleteRemote, false),
                    entry("test/Documents", Action::DeleteRemote, true),
                    entry("test/Documents/doc1.txt", Action::DeleteRemote, false),
                    entry("other.txt", Action::DeleteRemote, false),
                    entry("home", Action::DeleteLocal, true),
                    entry("home/inner", Action::DeleteLocal, true),
                    entry("home/inner/keep.txt", Action::DeleteLocal, false),
                    entry("home/keep.txt", Action::DeleteLocal, false),
                    entry("loose.txt", Action::DeleteLocal, false),
                },
        };
        const SharedData::Sync::SyncExecutionRequest request{
            .uploadId = Ids::generateOperationId(),
            .downloadId = Ids::generateOperationId(),
            .remoteDeleteId = Ids::generateOperationId(),
            .localDeleteId = Ids::generateOperationId(),
        };

        const auto result = runOnStrand([&]() {
            return queue->executeSyncPlan(*sftp, session, std::move(plan), request, *localQueue_);
        });
        EXPECT_EQ(result.remoteDeletes, (std::vector<std::string>{"other.txt"}));
        EXPECT_EQ(result.localDeletes, (std::vector<std::string>{"loose.txt"}));
        EXPECT_EQ(
            result.failed,
            (std::vector<std::string>{
                "test",
                "test/file2.txt",
                "test/Documents",
                "test/Documents/doc1.txt",
                "home",
                "home/inner",
                "home/inner/keep.txt",
                "home/keep.txt",
            })
        );

        setPaused(false);
        workUntilCompletions(2);
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("file2.txt"));
        EXPECT_FALSE(remoteNames(*sftp, "/home").contains("other.txt"));
        EXPECT_TRUE(std::filesystem::exists(localRoot / "home" / "keep.txt"));
        EXPECT_TRUE(std::filesystem::exists(localRoot / "home" / "inner" / "keep.txt"));
        EXPECT_FALSE(std::filesystem::exists(localRoot / "loose.txt"));
    }
#endif

    TEST_F(OperationQueueTests, CancelledDirectoryDownloadDuringScanLeavesNextUnitIntact)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto first = Ids::generateOperationId();
        const auto second = Ids::generateOperationId();
        const auto firstLocal = localDirectory_.path() / "first";
        const auto secondLocal = localDirectory_.path() / "second";
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, first, toLocal(firstLocal), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, second, toLocal(secondLocal), remote("/home/test/Pictures"), false, false, false, false
                );
            }
        ).has_value());

        runOnStrand([&]() {
            return queue->work();
        });
        queue->cancel(first);

        workUntilCompletions(2);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 2u);
        EXPECT_EQ(finished.front().operationId, first);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(finished.back().operationId, second);
        EXPECT_TRUE(localNames(firstLocal).empty());
        EXPECT_EQ(localNames(secondLocal), (std::set<std::string>{"image1.png", "image2.jpg"}));
    }

    TEST_F(OperationQueueTests, ACancelledOperationTellsItsOwnerItWasCanceled)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto deletion = Ids::generateOperationId();
        const auto heard = std::make_shared<std::vector<SharedData::OperationCompleted>>();
        ASSERT_TRUE(addOwnedDelete(*sftp, deletion, "/home/test/file1.txt", heard).has_value());
        queue->cancel(deletion);
        runOnStrand([]() {});

        ASSERT_EQ(heard->size(), 1u);
        EXPECT_EQ(heard->front().operationId, deletion);
        EXPECT_EQ(heard->front().reason, SharedData::OperationCompletionReason::Canceled);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("file1.txt"));
    }

    TEST_F(OperationQueueTests, CancellingAllTellsEveryWaitingOwner)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto first = Ids::generateOperationId();
        const auto second = Ids::generateOperationId();
        const auto heard = std::make_shared<std::vector<SharedData::OperationCompleted>>();
        ASSERT_TRUE(addOwnedDelete(*sftp, first, "/home/test/file1.txt", heard).has_value());
        ASSERT_TRUE(addOwnedDelete(*sftp, second, "/home/test/Documents/doc1.txt", heard).has_value());
        queue->cancelAll();
        runOnStrand([]() {});

        ASSERT_EQ(heard->size(), 2u);
        for (auto const& completed : *heard)
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(completions().size(), 2u);
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("file1.txt"));
    }

    TEST_F(OperationQueueTests, DroppingAllAtTeardownTellsNobody)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto heard = std::make_shared<std::vector<SharedData::OperationCompleted>>();
        ASSERT_TRUE(addOwnedDelete(*sftp, Ids::generateOperationId(), "/home/test/file1.txt", heard).has_value());
        queue->dropAll();
        runOnStrand([]() {});
        setPaused(false);
        runOnStrand([&]() {
            return queue->work();
        });

        EXPECT_TRUE(heard->empty());
        EXPECT_TRUE(completions().empty());
        EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("file1.txt"));
    }

    TEST_F(OperationQueueTests, AnOwnerCancellingFromItsCompletionLeavesTheOtherOperationsAlone)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        // Takes many steps, so the delete queued behind it completes first.
        const auto big = localDirectory_.path() / "big.bin";
        std::ofstream{big, std::ios::binary} << std::string(8u * 1024u * 1024u, 'x');
        const auto upload = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addUploadOperation(
                *sftp, upload, toLocal(big), remote("/home/test/big.bin"), true, false, false, false
            );
        }).has_value());
        const auto deletion = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            const auto added = queue->addDeleteOperation(*sftp, deletion, remote("/home/test/file1.txt"), false, false);
            queue->whenCompleted(
                deletion,
                [weak = std::weak_ptr{queue}, upload](SharedData::OperationCompleted const&)
                {
                    if (const auto owner = weak.lock())
                        owner->cancel(upload);
                }
            );
            return added;
        }).has_value());
        const auto later = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, later, remote("/home/test/Documents/doc2.txt"), false, false);
        }).has_value());
        setPaused(false);

        workUntilCompletions(3);
        std::map<std::string, SharedData::OperationCompletionReason> reasons;
        for (auto const& completed : completions())
            reasons.emplace(completed.operationId.value(), completed.reason);
        EXPECT_EQ(reasons.size(), 3u);
        EXPECT_EQ(reasons[upload.value()], SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(reasons[deletion.value()], SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(reasons[later.value()], SharedData::OperationCompletionReason::Completed);
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("big.bin"));
        EXPECT_FALSE(remoteNames(*sftp, "/home/test/Documents").contains("doc2.txt"));
    }

    TEST_F(OperationQueueTests, ReorderedUnitsKeepTheirOwnScans)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto first = Ids::generateOperationId();
        const auto second = Ids::generateOperationId();
        const auto firstLocal = localDirectory_.path() / "first";
        const auto secondLocal = localDirectory_.path() / "second";
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, first, toLocal(firstLocal), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, second, toLocal(secondLocal), remote("/home/test/Pictures"), false, false, false, false
                );
            }
        ).has_value());

        queue->moveOperation(second, 0);
        setPaused(false);

        workUntilCompletions(2);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 2u);
        EXPECT_EQ(finished[0].operationId, second);
        EXPECT_EQ(finished[1].operationId, first);
        EXPECT_EQ(localNames(firstLocal), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
        EXPECT_EQ(localNames(secondLocal), (std::set<std::string>{"image1.png", "image2.jpg"}));
    }

    TEST_F(OperationQueueTests, ScanFailureFailsTheUnit)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto download = Ids::generateOperationId();
        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp,
                    download,
                    toLocal(localDirectory_.path() / "Documents"),
                    remote("/home/test/Documents"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, remove, remote("/home/test/Documents"), true, false);
        }).has_value());
        const auto archive = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addArchiveDownloadOperation(
                *sftp,
                archive,
                {SharedData::ArchiveDownloadRoot{
                    .path = remote("/home/test/Documents"),
                    .entry = SharedData::DirectoryEntry{.name = "Documents", .type = SharedData::FileType::Directory},
                }},
                toLocal(localDirectory_.path() / "documents.tar"),
                TarArchive::Compression::None,
                {},
                false
            );
        }).has_value());

        workUntilCompletions(3);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 3u);
        for (auto const& completed : finished)
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Failed) << completed.operationId.value();
    }

    TEST_F(OperationQueueTests, RecursiveDeleteOfSymlinkRootLeavesTarget)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(
            serverStartResult->command("createSymlink", {{"path", "/home/test/linkdir"}, {"target", "/home/test/Documents"}})
        );
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, remove, remote("/home/test/linkdir"), true, false);
        }).has_value());

        workUntilCompletions(1);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Documents"), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
        EXPECT_FALSE(remoteNames(*sftp, "/home/test").contains("linkdir"));
    }

    TEST_F(OperationQueueTests, DeletesOfTheRootAndTheHomeDirectoryAreRefused)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto before = remoteNames(*sftp, "/home/test");
        for (auto const* target : {"/", "/home", "/home/test"})
        {
            const auto result = runOnStrand([&]() {
                return queue->addDeleteOperation(*sftp, Ids::generateOperationId(), remote(target), true, false);
            });
            ASSERT_FALSE(result.has_value()) << target;
            EXPECT_EQ(result.error().type, Operation::ErrorType::InvalidPath) << target;
        }
        const auto unset = runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, Ids::generateOperationId(), Paths::RemotePath{}, false, false);
        });
        EXPECT_FALSE(unset.has_value());

        const auto allowed = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, allowed, remote("/home/test/file2.txt"), false, false);
        }).has_value());
        workUntilCompletions(1);
        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().operationId, allowed);

        auto expected = before;
        expected.erase("file2.txt");
        EXPECT_EQ(remoteNames(*sftp, "/home/test"), expected);
    }

    TEST_F(OperationQueueTests, RecursiveDeleteRefusesScannedEntriesOutsideItsRoot)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);

        // A name leaving the root, and a second parentless entry that would stand for the root again.
        std::vector<std::vector<SharedData::DirectoryEntry>> hostileScans(2);
        hostileScans[0].resize(3);
        hostileScans[0][1].name = "doc1.txt";
        hostileScans[0][1].parent = 0;
        hostileScans[0][2].name = "../file1.txt";
        hostileScans[0][2].parent = 0;
        hostileScans[1].resize(2);
        hostileScans[1][1].name = "doc1.txt";

        for (auto& entries : hostileScans)
        {
            DeleteOperation operation{
                sftp,
                {
                    .remotePath = remote("/home/test/Documents"),
                    .recursive = true,
                }
            };
            operation.setScanResult(std::move(entries), 0);

            const auto result = operation.work();
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().type, Operation::ErrorType::InvalidPath);
            EXPECT_TRUE(remoteNames(*sftp, "/home/test").contains("file1.txt"));
            EXPECT_TRUE(remoteNames(*sftp, "/home/test/Documents").contains("doc1.txt"));
        }
    }

    TEST_F(OperationQueueTests, SyncScansAreDeliveredById)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localRoot = localDirectory_.path() / "syncLocal";
        std::filesystem::create_directories(localRoot);
        std::ofstream{localRoot / "local.txt"} << "local";

        const auto syncSessionId = Ids::makeSyncSessionId("sync-session");
        const auto remoteScan = Ids::generateOperationId();
        const auto localScan = Ids::generateOperationId();
        const auto download = Ids::generateOperationId();
        runOnStrand([&]() {
            queue->addSyncScanOperation(
                *sftp,
                syncSessionId,
                remoteScan,
                localScan,
                remote("/home/test/Music"),
                Paths::LocalPath::make(localRoot).value(),
                false,
                true,
                false
            );
        });
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp,
                    download,
                    toLocal(localDirectory_.path() / "Pictures"),
                    remote("/home/test/Pictures"),
                    false,
                    false,
                    false,
                    false,
                    SharedData::OperationMode::PriorityQueued
                );
            }
        ).has_value());

        workUntilCompletions(3);
        auto session = queue->syncSession(syncSessionId);
        ASSERT_TRUE(session);
        std::promise<bool> ready;
        boost::asio::post(session->strand(), [&]() {
            ready.set_value(session->bothTreesReadyInStrand());
        });
        EXPECT_TRUE(ready.get_future().get());
        EXPECT_EQ(
            localNames(localDirectory_.path() / "Pictures"), (std::set<std::string>{"image1.png", "image2.jpg"})
        );
    }

    TEST_F(OperationQueueTests, SyncScansExcludeTempFilesAndTheirAsides)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto suffix = Persistence::effectiveTempFileSuffix(std::nullopt);
        const auto localRoot = localDirectory_.path() / "syncAsides";
        std::filesystem::create_directories(localRoot);
        std::ofstream{localRoot / "keep.txt"} << "keep";
        std::ofstream{localRoot / fmt::format("part.txt{}", suffix)} << "part";
        std::ofstream{localRoot / fmt::format("old.txt{}", Persistence::asideFileSuffix(suffix))} << "old";

        const auto syncSessionId = Ids::generateSyncSessionId();
        runOnStrand(
            [&]()
            {
                queue->addSyncScanOperation(
                    *sftp,
                    syncSessionId,
                    Ids::generateOperationId(),
                    Ids::generateOperationId(),
                    remote("/home/test/Music"),
                    Paths::LocalPath::make(localRoot).value(),
                    false,
                    true,
                    false
                );
            }
        );
        workUntilCompletions(2);
        auto session = queue->syncSession(syncSessionId);
        ASSERT_TRUE(session);
        std::promise<SharedData::Sync::DiffSummary> summary;
        boost::asio::post(
            session->strand(),
            [&]()
            {
                summary.set_value(
                    session->recomputeDiffInStrand({.direction = SharedData::Sync::Direction::Upload}, {})
                );
            }
        );
        const auto result = summary.get_future().get();
        std::set<std::string> excluded;
        for (auto const& blocked : result.blockedSamples)
        {
            if (blocked.reason == SharedData::Sync::BlockReason::Excluded)
                excluded.insert(blocked.relKey);
        }
        EXPECT_EQ(
            excluded,
            (std::set<std::string>{
                fmt::format("part.txt{}", suffix), fmt::format("old.txt{}", Persistence::asideFileSuffix(suffix))
            })
        );
        EXPECT_EQ(result.uploads.itemCount, 1u) << "keep.txt";
    }

    TEST_F(OperationQueueTests, AFailedSyncScanCancelsTheOtherOne)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("denyDirectory", {{"path", "/home/test/Documents"}}));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();

        const auto localRoot = localDirectory_.path() / "syncSibling";
        for (int directory = 0; directory < 50; ++directory)
        {
            const auto path = localRoot / fmt::format("d{}", directory);
            std::filesystem::create_directories(path);
            for (int file = 0; file < 20; ++file)
                std::ofstream{path / fmt::format("f{}", file)} << "x";
        }

        const auto remoteScan = Ids::generateOperationId();
        const auto localScan = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                queue->addSyncScanOperation(
                    *sftp,
                    Ids::generateSyncSessionId(),
                    remoteScan,
                    localScan,
                    remote("/home/test/Documents"),
                    Paths::LocalPath::make(localRoot).value(),
                    false,
                    true,
                    false
                );
            }
        );
        workUntilCompletions(2);
        std::map<std::string, SharedData::OperationCompletionReason> reasons;
        for (auto const& completed : completions())
            reasons.emplace(completed.operationId.value(), completed.reason);
        EXPECT_EQ(reasons.at(remoteScan.value()), SharedData::OperationCompletionReason::Failed);
        EXPECT_EQ(reasons.at(localScan.value()), SharedData::OperationCompletionReason::Canceled);
    }

    TEST_F(OperationQueueTests, HostileNamesNeverBecomeLocalPaths)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        // A file literally named "x/.." would turn into the download root's parent when joined as a path.
        ASSERT_TRUE(serverStartResult->command(
            "rawName", {{"directory", "/home/test/Documents"}, {"name", "x/.."}, {"content", "hostile"}}
        ));
        ASSERT_TRUE(serverStartResult->command(
            "rawName", {{"directory", "/home/test/Documents"}, {"name", ".."}, {"content", "hostile"}}
        ));
        ASSERT_TRUE(serverStartResult->command(
            "rawName", {{"directory", "/home/test/Documents"}, {"name", "a\\b"}, {"content", "backslash"}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "inner" / "Documents";
        std::filesystem::create_directories(localPath.parent_path());
        const auto download = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, download, toLocal(localPath), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());

        workUntilCompletions(1);
        const auto finished = completions().front();
        // Partial success: the frontend reads failed entries of a completed bulk as such. On Windows "a\b" cannot be
        // stored either.
#ifdef _WIN32
        ASSERT_EQ(finished.failedEntries.size(), 2u);
#else
        ASSERT_EQ(finished.failedEntries.size(), 1u);
#endif
        for (auto const& [path, error] : finished.failedEntries)
            EXPECT_EQ(error.type, SharedData::OperationErrorType::InvalidName) << path;
        // Nothing was written next to or above the download root.
        EXPECT_EQ(localNames(localPath.parent_path()), (std::set<std::string>{"Documents"}));
        EXPECT_EQ(localNames(localDirectory_.path()), (std::set<std::string>{"inner"}));
#ifdef _WIN32
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
#else
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"a\\b", "doc1.txt", "doc2.txt"}));
#endif
    }

    TEST_F(OperationQueueTests, NamesThatAreNotUtf8AreRefusedAndTheCompletionStillSerializes)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command(
            "rawName", {{"directory", "/home/test/Documents"}, {"nameHex", "4672616ee76f6973"}, {"content", "latin1"}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "Documents";
        const auto download = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp, download, toLocal(localPath), remote("/home/test/Documents"), false, false, false, false
                );
            }
        ).has_value());

        workUntilCompletions(1);
        const auto finished = completions().front();
        ASSERT_EQ(finished.failedEntries.size(), 1u);
        for (auto const& [path, error] : finished.failedEntries)
        {
            EXPECT_EQ(error.type, SharedData::OperationErrorType::InvalidName) << path;
            EXPECT_TRUE(Paths::isValidUtf8(path));
            EXPECT_NE(path.find("Fran\xEF\xBF\xBDois"), std::string::npos) << path;
        }
        const nlohmann::json serialized = finished;
        EXPECT_NO_THROW(std::ignore = serialized.dump());
        EXPECT_EQ(localNames(localPath), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, NonAsciiNamesSurviveDownloadAndUpload)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/Grüße"}}));
        ASSERT_TRUE(serverStartResult->command(
            "createFile", {{"path", "/home/test/Grüße/東京 \xF0\x9F\x9A\x80.txt"}, {"content", "tokyo"}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto downloaded = localDirectory_.path() / Paths::nativeFromUtf8("Grüße");
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addDownloadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(downloaded),
                    remote("/home/test/Grüße"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        workUntilCompletions(1);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(localNames(downloaded), (std::set<std::string>{"東京 \xF0\x9F\x9A\x80.txt"}));
        std::ifstream downloadedFile{downloaded / Paths::nativeFromUtf8("東京 \xF0\x9F\x9A\x80.txt")};
        const std::string downloadedContent{std::istreambuf_iterator<char>{downloadedFile}, {}};
        EXPECT_EQ(downloadedContent, "tokyo");

        const auto toUpload = localDirectory_.path() / Paths::nativeFromUtf8("Jürgen");
        std::filesystem::create_directories(toUpload / Paths::nativeFromUtf8("Ärger"));
        std::ofstream{toUpload / Paths::nativeFromUtf8("Ärger") / Paths::nativeFromUtf8("Größe.txt")} << "size";
        ASSERT_TRUE(runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(toUpload),
                    remote("/home/test/Jürgen"),
                    false,
                    false,
                    false,
                    false
                );
            }
        ).has_value());
        workUntilCompletions(2);
        EXPECT_EQ(completions().back().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Jürgen"), (std::set<std::string>{"Ärger"}));
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Jürgen/Ärger"), (std::set<std::string>{"Größe.txt"}));
    }

    TEST_F(OperationQueueTests, RecursiveDeleteNeverFollowsHostileNamesOut)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        ASSERT_TRUE(serverStartResult->command("createDirectory", {{"path", "/home/test/victim"}}));
        ASSERT_TRUE(serverStartResult->command("createFile", {{"path", "/home/test/victim/keep.txt"}, {"content", "x"}}));
        ASSERT_TRUE(serverStartResult->command(
            "rawName", {{"directory", "/home/test/victim"}, {"name", "x/.."}, {"type", "directory"}}
        ));
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto before = remoteNames(*sftp, "/home/test");
        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(runOnStrand([&]() {
            return queue->addDeleteOperation(*sftp, remove, remote("/home/test/victim"), true, false);
        }).has_value());

        workUntilCompletions(1);
        // The hostile entry stays, so the directory cannot be removed, but nothing outside it was touched.
        auto after = remoteNames(*sftp, "/home/test");
        after.insert("victim");
        EXPECT_EQ(after, before);
        EXPECT_EQ(remoteNames(*sftp, "/home/test/Documents"), (std::set<std::string>{"doc1.txt", "doc2.txt"}));
    }

    TEST_F(OperationQueueTests, ADirectoryUploadInTheUnsafeFastModeRenamesNothing)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        queue->unsafeFast(true);
        setPaused(false);

        const auto localPath = localDirectory_.path() / "fast";
        std::filesystem::create_directories(localPath);
        for (auto const* name : {"a.txt", "b.txt", "c.txt"})
            std::ofstream{localPath / name} << name;

        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp, Ids::generateOperationId(), toLocal(localPath), remote("/home/test/fast"), false, false, false, false
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();
        workUntilCompletions(1);
        const auto counts = serverStartResult->query("requestCounts");
        ASSERT_TRUE(counts.has_value());

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(counts->value("RENAME", 0) + counts->value("EXTENDED", 0), 0) << counts->dump();
        EXPECT_EQ(remoteNames(*sftp, "/home/test/fast"), (std::set<std::string>{"a.txt", "b.txt", "c.txt"}));
    }

    TEST_F(OperationQueueTests, AnUploadThatTellsWhatItSentStaysSafeInTheUnsafeFastMode)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        queue->unsafeFast(true);
        setPaused(false);
        std::ofstream{localDirectory_.path() / "tracked.txt"} << "tracked";

        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addUploadOperation(
                    *sftp,
                    Ids::generateOperationId(),
                    toLocal(localDirectory_.path() / "tracked.txt"),
                    remote("/home/test/tracked.txt"),
                    false,
                    false,
                    false,
                    false,
                    SharedData::OperationMode::Queued,
                    false,
                    std::nullopt,
                    [](Paths::LocalPath const&, std::optional<UploadOperation::SentSource> const&) {}
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();
        workUntilCompletions(1);
        const auto counts = serverStartResult->query("requestCounts");
        ASSERT_TRUE(counts.has_value());

        EXPECT_GE(counts->value("RENAME", 0) + counts->value("EXTENDED", 0), 1) << counts->dump();
    }

    TEST_F(OperationQueueTests, TheUnsafeFastModeTurnedOnForARunningBulkAppliesToTheFilesItOpensAfterwards)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
        setPaused(false);

        const auto localPath = localDirectory_.path() / "switched";
        std::filesystem::create_directories(localPath);
        std::ofstream{localPath / "first.bin"} << std::string(256 * 1024, 'x');
        std::ofstream{localPath / "second.txt"} << "second";
        const SharedData::BulkUploadRequest request{
            .entries = {
                {.source = toLocal(localPath / "first.bin"),
                 .destination = remote("/home/test/first.bin"),
                 .sizeBytes = 256 * 1024},
                {.source = toLocal(localPath / "second.txt"), .destination = remote("/home/test/second.txt"), .sizeBytes = 6},
            },
        };

        ASSERT_TRUE(serverStartResult->command("resetRequestCounts", {}));
        runOnStrand(
            [&]()
            {
                queue->addBulkUploadOperation(
                    *sftp,
                    request,
                    [](std::size_t)
                    {
                        return Ids::generateOperationId();
                    },
                    Ids::generateOperationId()
                );
            }
        );
        // The first file opens safely.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!remoteNames(*sftp, "/home/test").contains("first.bin.filepart"))
        {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            runOnStrand(
                [&]()
                {
                    return queue->work();
                }
            );
        }
        queue->unsafeFast(true);
        workUntilCompletions(1);
        const auto counts = serverStartResult->query("requestCounts");
        ASSERT_TRUE(counts.has_value());

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(counts->value("RENAME", 0) + counts->value("EXTENDED", 0), 1) << counts->dump();
        EXPECT_EQ(remoteNames(*sftp, "/home/test").count("first.bin.filepart"), 0u);
    }
}
