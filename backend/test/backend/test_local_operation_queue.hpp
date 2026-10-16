#pragma once

#include <backend/local_delete_guard.hpp>
#include <backend/local_operation_queue.hpp>
#include <paths/native.hpp>
#include <utility/localized_message.hpp>
#include <utility/temporary_directory.hpp>
#include "nui_env.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class LocalOperationQueueTests : public ::testing::Test
    {
      public:
        void SetUp() override
        {
            if (!NuiEnv::instance().available())
                GTEST_SKIP() << "No display available; skipping LocalOperationQueue tests";
            auto& env = NuiEnv::instance();
            queue_ = std::make_shared<LocalOperationQueue>(
                pool_.get_executor(), strand_, *env.window, *env.hub, deletionPrevented_
            );
            queue_->setCompletionObserver(
                [this](SharedData::OperationCompleted const& completed)
                {
                    std::scoped_lock lock{completionsMutex_};
                    completions_.push_back(completed);
                }
            );
        }

        void TearDown() override
        {
            queue_.reset();
        }

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

        std::expected<void, Operation::Error> add(LocalOperationQueue::DeleteRequest request)
        {
            return runOnStrand(
                [this, &request]()
                {
                    return queue_->addDelete(std::move(request));
                }
            );
        }

        /**
         * @brief The completions once @p expected arrived, the queue working on its own; fails after a while.
         */
        std::vector<SharedData::OperationCompleted> waitForCompletions(std::size_t expected)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (auto finished = completions(); finished.size() >= expected)
                    return finished;
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            ADD_FAILURE() << "Timed out waiting for " << expected << " completions";
            return completions();
        }

        /**
         * @brief The completions after the queue had time to finish what it may run.
         */
        std::vector<SharedData::OperationCompleted> completionsAfterSettling()
        {
            for (int cycle = 0; cycle < 20; ++cycle)
            {
                runOnStrand([]() {});
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return completions();
        }

        std::vector<SharedData::OperationCompleted> completions()
        {
            std::scoped_lock lock{completionsMutex_};
            return completions_;
        }

        static Paths::LocalPath local(std::filesystem::path const& path)
        {
            return Paths::LocalPath::make(path).value();
        }

        std::filesystem::path makeTree(std::string const& name)
        {
            const auto tree = root_.path() / name;
            std::filesystem::create_directories(tree / "inner");
            std::ofstream{tree / "inner" / "a.txt"} << "a";
            std::ofstream{tree / "b.txt"} << "b";
            return tree;
        }

      protected:
        boost::asio::thread_pool pool_{2};
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_{
            std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(pool_.get_executor())
        };
        std::shared_ptr<std::atomic_bool> deletionPrevented_{std::make_shared<std::atomic_bool>(false)};
        std::shared_ptr<LocalOperationQueue> queue_{};
        Ids::SessionId owner_{Ids::makeSessionId("local-operation-queue-test")};
        std::mutex completionsMutex_{};
        std::vector<SharedData::OperationCompleted> completions_{};
        Utility::TemporaryDirectory root_{temporaryRoot, true};
    };

    TEST_F(LocalOperationQueueTests, AFlatDeleteRemovesExactlyItsTargets)
    {
        const auto root = root_.path() / "flat";
        std::filesystem::create_directories(root / "directory" / "inner");
        std::ofstream{root / "directory" / "listed.txt"} << "listed";
        std::ofstream{root / "directory" / "inner" / "unlisted.txt"} << "unlisted";
        std::ofstream{root / "file.txt"} << "file";
        std::ofstream{root / "keep.txt"} << "keep";

        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(add({
                            .operationId = remove,
                            .targets =
                                {local(root / "directory"),
                                    local(root / "directory" / "inner"),
                                    local(root / "directory" / "listed.txt"),
                                    local(root / "file.txt")},
                        })
                .has_value());

        const auto finished = waitForCompletions(1);
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, remove);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(finished.front().failedEntries.size(), 2u) << "inner and directory are not empty";
        EXPECT_TRUE(std::filesystem::exists(root / "directory" / "inner" / "unlisted.txt"));
        EXPECT_FALSE(std::filesystem::exists(root / "directory" / "listed.txt"));
        EXPECT_FALSE(std::filesystem::exists(root / "file.txt"));
        EXPECT_TRUE(std::filesystem::exists(root / "keep.txt"));
    }

#ifndef _WIN32
    TEST_F(LocalOperationQueueTests, ARecursiveDeleteRemovesTheTreeWithoutFollowingLinks)
    {
        const auto outside = root_.path() / "outside";
        std::filesystem::create_directories(outside);
        std::ofstream{outside / "keep.txt"} << "keep";
        const auto tree = makeTree("recursive");
        std::filesystem::create_directory_symlink(outside, tree / "inner" / "out");

        ASSERT_TRUE(
            add({.operationId = Ids::generateOperationId(), .targets = {local(tree)}, .recursive = true}).has_value()
        );

        const auto finished = waitForCompletions(1);
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_TRUE(finished.front().failedEntries.empty());
        EXPECT_FALSE(std::filesystem::exists(tree));
        EXPECT_TRUE(std::filesystem::exists(outside / "keep.txt"));
    }
#endif

    TEST_F(LocalOperationQueueTests, AHeldDeleteWaitsUntilItsOwnerContinuesOrEnds)
    {
        const auto first = makeTree("first");
        const auto second = makeTree("second");
        const auto unowned = makeTree("unowned");
        const auto firstId = Ids::generateOperationId();
        const auto secondId = Ids::generateOperationId();
        const auto unownedId = Ids::generateOperationId();
        ASSERT_TRUE(
            add({.operationId = firstId, .targets = {local(first)}, .recursive = true, .owner = owner_, .held = true})
                .has_value()
        );
        ASSERT_TRUE(
            add({.operationId = secondId, .targets = {local(second)}, .recursive = true, .owner = owner_, .held = true})
                .has_value()
        );
        ASSERT_TRUE(add({.operationId = unownedId, .targets = {local(unowned)}, .recursive = true}).has_value());

        auto finished = waitForCompletions(1);
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, unownedId) << "a held delete does not stop the others";
        EXPECT_TRUE(std::filesystem::exists(first / "b.txt"));

        runOnStrand(
            [this]()
            {
                queue_->hold(owner_, false);
                queue_->hold(owner_, true);
            }
        );
        EXPECT_EQ(completionsAfterSettling().size(), 1u);

        runOnStrand(
            [this]()
            {
                queue_->hold(owner_, false);
            }
        );
        finished = waitForCompletions(2);
        ASSERT_GE(finished.size(), 2u);
        EXPECT_EQ(finished[1].operationId, firstId);

        // A delete of a session that ended runs on its own and is not held again.
        ASSERT_TRUE(add({.operationId = Ids::generateOperationId(),
                            .targets = {local(makeTree("third"))},
                            .recursive = true,
                            .owner = owner_,
                            .held = true})
                .has_value());
        runOnStrand(
            [this]()
            {
                queue_->hold(owner_, true);
                queue_->release(owner_);
                queue_->hold(owner_, true);
            }
        );
        finished = waitForCompletions(4);
        EXPECT_EQ(finished.size(), 4u);
        EXPECT_FALSE(std::filesystem::exists(second));
        EXPECT_FALSE(std::filesystem::exists(root_.path() / "third"));
    }

    TEST_F(LocalOperationQueueTests, APriorityDeleteIsNeverHeldAndRunsFirst)
    {
        const auto queued = makeTree("queued");
        const auto priority = makeTree("priority");
        const auto queuedId = Ids::generateOperationId();
        const auto priorityId = Ids::generateOperationId();
        runOnStrand(
            [&]()
            {
                EXPECT_TRUE(queue_->addDelete({.operationId = queuedId, .targets = {local(queued)}, .recursive = true})
                        .has_value());
                EXPECT_TRUE(queue_
                        ->addDelete({
                            .operationId = priorityId,
                            .targets = {local(priority)},
                            .recursive = true,
                            .mode = SharedData::OperationMode::PriorityQueued,
                            .owner = owner_,
                            .held = true,
                        })
                        .has_value());
            }
        );

        const auto finished = waitForCompletions(2);
        ASSERT_EQ(finished.size(), 2u);
        EXPECT_EQ(finished[0].operationId, priorityId);
        EXPECT_EQ(finished[1].operationId, queuedId);
    }

    TEST_F(LocalOperationQueueTests, ACanceledDeleteEndsAsCanceledAndKeepsWhatIsLeft)
    {
        const auto tree = makeTree("canceled");
        const auto remove = Ids::generateOperationId();
        ASSERT_TRUE(
            add({.operationId = remove, .targets = {local(tree)}, .recursive = true, .owner = owner_, .held = true})
                .has_value()
        );
        runOnStrand(
            [&]()
            {
                queue_->cancel(remove);
                queue_->cancel(remove);
            }
        );

        const auto finished = completionsAfterSettling();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, remove);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Canceled);
        EXPECT_TRUE(std::filesystem::exists(tree / "b.txt"));
    }

    TEST_F(LocalOperationQueueTests, ADeleteOfARootOrTheHomeDirectoryIsRefused)
    {
        const auto file = root_.path() / "survivor.txt";
        std::ofstream{file} << "survivor";
        const auto root = local(std::filesystem::path{file}.root_path());
        const auto homes = localHomeDirectories();
        ASSERT_FALSE(homes.empty());

        for (auto const& target : {root, homes.front()})
        {
            const auto result = add({.operationId = Ids::generateOperationId(), .targets = {local(file), target}});
            ASSERT_FALSE(result.has_value()) << target.toUtf8();
            EXPECT_EQ(result.error().type, Operation::ErrorType::InvalidPath);
        }
        EXPECT_FALSE(add({.operationId = Ids::generateOperationId()}).has_value()) << "no targets";
        EXPECT_TRUE(completionsAfterSettling().empty());
        EXPECT_TRUE(std::filesystem::exists(file));
    }

    TEST_F(LocalOperationQueueTests, AnExplorerDeleteGoesWithTheContentUnlessRefused)
    {
        const auto tree = makeTree("explorer");
        const auto explorerDelete = [this](std::vector<std::string> const& paths)
        {
            return runOnStrand(
                [this, &paths]()
                {
                    return queue_->addExplorerDelete(Ids::generateOperationId(), paths, std::nullopt, false);
                }
            );
        };

        deletionPrevented_->store(true);
        const auto prevented = explorerDelete({Paths::nativeToUtf8(tree)});
        ASSERT_FALSE(prevented.has_value());
        EXPECT_EQ(prevented.error(), "backend.rpcFilesystem.deletionPrevented");
        deletionPrevented_->store(false);

        const auto relative = explorerDelete({Paths::nativeToUtf8(tree), "relative/path"});
        ASSERT_FALSE(relative.has_value());
        EXPECT_EQ(Utility::parseLocalizedMessage(relative.error()).key, "backend.rpcFilesystem.removeRefused");
        EXPECT_TRUE(completionsAfterSettling().empty());
        EXPECT_TRUE(std::filesystem::exists(tree / "inner" / "a.txt"));

        ASSERT_TRUE(explorerDelete({Paths::nativeToUtf8(tree)}).has_value());
        const auto finished = waitForCompletions(1);
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
        EXPECT_FALSE(std::filesystem::exists(tree));
    }
}
