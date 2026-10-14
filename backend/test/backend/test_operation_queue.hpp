#pragma once

#include <backend/local_delete_guard.hpp>
#include <backend/sftp/operation_queue.hpp>
#include <backend/sftp/sequential_scan_progress.hpp>
#include "nui_env.hpp"
#include "real_server_tests.hpp"

#include <nui/utility/scope_exit.hpp>
#include <paths/native.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
                    queue_->cancelAll();
                });
            queue_.reset();
        }

        std::shared_ptr<OperationQueue> makeQueue()
        {
            auto& env = NuiEnv::instance();
            queue_ = std::make_shared<OperationQueue>(
                queuePool_.get_executor(),
                strand_,
                *env.window,
                *env.hub,
                Persistence::SftpOptions{},
                Ids::makeSessionId("operation-queue-test"),
                2
            );
            queue_->setCompletionObserver(
                [this](SharedData::OperationCompleted const& completed)
                {
                    std::scoped_lock lock{completionsMutex_};
                    completions_.push_back(completed);
                }
            );
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

        void setPaused(bool pause)
        {
            queue_->paused(pause);
            runOnStrand([]() {});
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

        std::vector<SharedData::OperationCompleted> completions()
        {
            std::scoped_lock lock{completionsMutex_};
            return completions_;
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
                    queue_->cancelAll();
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

      protected:
        boost::asio::thread_pool queuePool_{2};
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_{
            std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(queuePool_.get_executor())};
        std::shared_ptr<OperationQueue> queue_{};
        std::mutex completionsMutex_{};
        std::vector<SharedData::OperationCompleted> completions_{};
        Utility::TemporaryDirectory localDirectory_{temporaryRoot, true};
    };

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

#ifndef _WIN32
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
                0,
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

    TEST_F(OperationQueueTests, DirectoryUploadCreatesDirectoriesWithTheDefaultMode)
    {
        CREATE_SERVER_AND_JOINER(sftpServer);
        auto [_, sftp] = createSftpSession(serverStartResult->port);
        auto queue = makeQueue();
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
        const auto modeOf = [&](std::string const& path) {
            auto future = sftp->stat(remote(path));
            if (future.wait_for(5s) != std::future_status::ready)
                return perms::unknown;
            const auto information = future.get();
            return information ? information->permissions & perms::mask : perms::unknown;
        };
        // Not inherited by default: owner everything, group read and enter, never world writable.
        const auto expected = perms::owner_all | perms::group_read | perms::group_exec;
        EXPECT_EQ(modeOf("/home/test/modes"), expected);
        EXPECT_EQ(modeOf("/home/test/modes/sub"), expected);
    }

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
                queue->cancelAll();
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

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, fileDelete);
        EXPECT_EQ(finished.front().reason, SharedData::OperationCompletionReason::Completed);
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
        const auto next = Detail::sequentialScanProgress(
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
        const auto entry = [&session](std::string relKey, Action action, bool isDirectory, std::uint64_t size = 0)
        {
            auto paths = session.resolve(Paths::RelativePath::make(relKey).value()).value();
            return SyncSession::ResolvedPlanEntry{
                .entry = {.relKey = std::move(relKey), .action = action, .sizeBytes = size, .isDirectory = isDirectory},
                .paths = std::move(paths),
            };
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
            return queue->executeSyncPlan(*sftp, session, std::move(plan), request);
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

        workUntilCompletions(1);
        const auto finished = completions();
        ASSERT_EQ(finished.size(), 1u);
        EXPECT_EQ(finished.front().operationId, second);
        EXPECT_TRUE(localNames(firstLocal).empty());
        EXPECT_EQ(localNames(secondLocal), (std::set<std::string>{"image1.png", "image2.jpg"}));
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
                0,
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
                *sftp,
                {
                    .remotePath = remote("/home/test/Documents"),
                    .recursive = true,
                }};
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
}
