#pragma once

#include "test_operation_queue_batches.hpp"

#include <backend/sftp/scan_operation.hpp>
#include <persistence/state/sftp_options.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <set>
#include <string>

namespace Test
{
    /**
     * @brief Operations on a file system whose connections are all taken, see FakeRemoteFileSystem::takeAllConnections.
     */
    class ConnectionWaitTests : public OperationQueueBatchTests
    {
      protected:
        /**
         * @brief A running queue whose wake counts how often it was woken: by adds and by the file system.
         */
        std::shared_ptr<OperationQueue> makeWakingQueue()
        {
            auto queue = makeQueue();
            runOnStrand(
                [this, &queue]()
                {
                    queue->setWake(
                        [this]()
                        {
                            ++wakes_;
                        }
                    );
                }
            );
            setPaused(false);
            return queue;
        }

        /**
         * @brief Works @p queue until a cycle wants no more work, at most a few cycles.
         * @return Whether it stopped wanting work.
         */
        bool workUntilWaiting(OperationQueue& queue)
        {
            for (int cycle = 0; cycle < 20; ++cycle)
            {
                if (!workCycle(queue))
                    return true;
            }
            return false;
        }

        Paths::LocalPath writeLocalFile(std::string const& name, std::string const& content)
        {
            const auto path = localDirectory_.path() / name;
            std::ofstream{path, std::ios::binary} << content;
            return toLocal(path);
        }

        Ids::OperationId
        queueUpload(OperationQueue& queue, Paths::LocalPath const& local, Paths::RemotePath const& target)
        {
            auto operationId = Ids::generateOperationId();
            const auto added = runOnStrand(
                [&]()
                {
                    return queue.addUploadOperation(fake_, operationId, local, target, false, false, false, false);
                }
            );
            EXPECT_TRUE(added.has_value()) << added.error().toString();
            return operationId;
        }

        static std::string uploadedContent()
        {
            std::string content;
            for (int index = 0; content.size() < 8u * 1024u; ++index)
                content += fmt::format("line {}\n", index);
            return content;
        }

        /**
         * @brief Puts the first half of @p content on the fake as the part an earlier upload to @p target left.
         * @return The path of the part.
         */
        Paths::RemotePath addPart(Paths::RemotePath const& target, std::string const& content)
        {
            const auto part = remote(fmt::format("{}{}", target.value(), Persistence::defaultTempFileSuffix));
            fake_->addFile(part, content.substr(0, content.size() / 2));
            return part;
        }

        /**
         * @brief Fails every open that truncates, as starting over does with the part.
         */
        void failStartingOver()
        {
            fake_->failWhen<RemoteIo::OpenFile>(
                [](RemoteIo::OpenFile const& open)
                {
                    return RemoteIo::contains(open.mode, RemoteIo::OpenMode::Truncate);
                },
                RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::Failure, .message = "The upload started over"}
            );
        }

        /**
         * @brief The names a scan of @p root on the fake found, its ignore files respected.
         */
        std::set<std::string> scannedNames(Paths::RemotePath const& root)
        {
            ScanOperation scan{
                fake_,
                ScanOperation::ScanOperationOptions{
                    .remotePath = root,
                    .respectIgnoreFiles = true,
                }
            };
            auto status = scan.work();
            for (int step = 0; status.has_value() && *status != Operation::WorkStatus::Complete && step < 100; ++step)
                status = scan.work();
            EXPECT_TRUE(status.has_value()) << status.error().toString();
            EXPECT_EQ(status.value_or(Operation::WorkStatus::MoreWork), Operation::WorkStatus::Complete);
            std::set<std::string> names;
            for (auto const& entry : scan.ejectEntries())
                names.insert(entry.name);
            return names;
        }

        void expectCompleted()
        {
            ASSERT_EQ(completions().size(), 1u);
            EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Completed)
                << (completions().front().error ? completions().front().error->toString() : std::string{});
        }

        std::atomic_int wakes_{0};
    };

    TEST_F(ConnectionWaitTests, ADownloadWaitsWithoutFailingUntilAConnectionIsFree)
    {
        auto queue = makeWakingQueue();
        fake_->takeAllConnections();
        const auto operationId = addDownload(*queue, "file.txt");

        EXPECT_TRUE(workUntilWaiting(*queue)) << "the queue kept working a download that waits";
        EXPECT_TRUE(completions().empty()) << "the download ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_EQ(completions().front().operationId, operationId);
        EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / "file.txt"));
    }

    TEST_F(ConnectionWaitTests, AWaitingDownloadOpensAgainOnlyOnceItWasWoken)
    {
        auto queue = makeWakingQueue();
        const auto waiting = addRemoteFile("waiting.txt");
        fake_->takeConnectionsFor(
            [waiting](RemoteIo::OpenFile const& open)
            {
                return open.path == waiting;
            }
        );
        addDownload(*queue, "running.txt");
        queueDownload(*queue, "waiting.txt");

        // The running download takes several reads, the queue works the waiting one in each of those cycles too.
        workUntilCompletions(1);
        EXPECT_EQ(fake_->busyOpens(), 1u) << "the waiting download opened again before it was woken";

        fake_->freeConnections();
        workUntilCompletions(2);

        ASSERT_EQ(completions().size(), 2u);
        for (auto const& completed : completions())
            EXPECT_EQ(completed.reason, SharedData::OperationCompletionReason::Completed)
                << (completed.error ? completed.error->toString() : std::string{});
        EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / "waiting.txt"));
    }

    TEST_F(ConnectionWaitTests, AnUploadWaitsWithoutFailingUntilAConnectionIsFree)
    {
        auto queue = makeWakingQueue();
        fake_->addDirectory(remote("/home/test"));
        const auto target = remote("/home/test/uploaded.txt");
        fake_->takeAllConnections();
        queueUpload(*queue, writeLocalFile("uploaded.txt", uploadedContent()), target);

        EXPECT_TRUE(workUntilWaiting(*queue)) << "the queue kept working an upload that waits";
        EXPECT_TRUE(completions().empty()) << "the upload ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_EQ(fake_->contentOf(target), uploadedContent());
    }

    TEST_F(ConnectionWaitTests, AnUploadWhoseTailCheckFindsEveryConnectionTakenContinuesItsPart)
    {
        auto queue = makeWakingQueue();
        const auto target = remote("/home/test/uploaded.txt");
        const auto content = uploadedContent();
        addPart(target, content);
        failStartingOver();
        fake_->takeAllConnections();
        queueUpload(*queue, writeLocalFile("uploaded.txt", content), target);

        EXPECT_TRUE(workUntilWaiting(*queue));
        EXPECT_TRUE(completions().empty()) << "the upload ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        fake_->freeConnections();
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_EQ(fake_->contentOf(target), content);
    }

    TEST_F(ConnectionWaitTests, AnUploadWhoseContinueWaitsForAConnectionChecksItsPartOnce)
    {
        auto queue = makeWakingQueue();
        const auto target = remote("/home/test/uploaded.txt");
        const auto content = uploadedContent();
        const auto part = addPart(target, content);
        failStartingOver();
        const auto tailChecks = std::make_shared<std::atomic_int>(0);
        fake_->failWhen<RemoteIo::OpenFile>(
            [part, tailChecks](RemoteIo::OpenFile const& open)
            {
                if (open.path == part && open.mode == RemoteIo::OpenMode::Read)
                    ++*tailChecks;
                return false;
            },
            {}
        );
        // The connection the tail check gave back is not free yet when the part is opened to continue it.
        fake_->takeConnectionsFor(
            [part](RemoteIo::OpenFile const& open)
            {
                return open.path == part && RemoteIo::contains(open.mode, RemoteIo::OpenMode::Write);
            }
        );
        queueUpload(*queue, writeLocalFile("uploaded.txt", content), target);

        EXPECT_TRUE(workUntilWaiting(*queue));
        EXPECT_TRUE(completions().empty()) << "the upload ended while its part waited for a connection";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        fake_->freeConnections();
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_EQ(fake_->contentOf(target), content);
        EXPECT_EQ(tailChecks->load(), 1) << "the part's tail was checked again after the wait";
    }

    TEST_F(ConnectionWaitTests, ABulkDownloadWaitsWithoutFailingUntilAConnectionIsFree)
    {
        auto queue = makeWakingQueue();
        const std::vector<std::string> names{"a.txt", "b.txt", "c.txt"};
        for (auto const& name : names)
            addRemoteFile(name);
        fake_->takeAllConnections();
        queueBulkDownload(*queue, names);

        EXPECT_TRUE(workUntilWaiting(*queue)) << "the queue kept working a bulk whose files wait";
        EXPECT_TRUE(completions().empty()) << "the bulk ended while every connection was taken";
        EXPECT_GE(fake_->busyOpens(), 1u);
        for (auto const& name : names)
            EXPECT_LE(fake_->busyOpensOf(remote(fmt::format("/home/test/{}", name))), 1u)
                << name << " opened again before it was woken";

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_TRUE(completions().front().failedEntries.empty());
        for (auto const& name : names)
            EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / name)) << name;
    }

    TEST_F(ConnectionWaitTests, AnUploadWhosePartCannotBeCheckedForALostConnectionKeepsIt)
    {
        auto queue = makeWakingQueue();
        const auto target = remote("/home/test/uploaded.txt");
        const auto content = uploadedContent();
        const auto part = addPart(target, content);
        fake_->failWhen<RemoteIo::OpenFile>(
            [part](RemoteIo::OpenFile const& open)
            {
                return open.path == part && open.mode == RemoteIo::OpenMode::Read;
            },
            RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::ConnectionLost, .message = "The connection broke"}
        );
        queueUpload(*queue, writeLocalFile("uploaded.txt", content), target);

        workUntilCompletions(1);

        ASSERT_EQ(completions().size(), 1u);
        EXPECT_EQ(completions().front().reason, SharedData::OperationCompletionReason::Failed);
        ASSERT_TRUE(completions().front().error.has_value());
        ASSERT_TRUE(completions().front().error->remoteError.has_value());
        EXPECT_EQ(completions().front().error->remoteError->kind, RemoteIo::RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(fake_->contentOf(part), content.substr(0, content.size() / 2)) << "the part was started over";
    }

    TEST_F(ConnectionWaitTests, AnUploadWhosePartCannotBeReadStartsOver)
    {
        auto queue = makeWakingQueue();
        const auto target = remote("/home/test/uploaded.txt");
        const auto content = uploadedContent();
        const auto part = addPart(target, content);
        // A server that only takes uploads never lets the part be read back.
        fake_->failWhen<RemoteIo::OpenFile>(
            [part](RemoteIo::OpenFile const& open)
            {
                return open.path == part && open.mode == RemoteIo::OpenMode::Read;
            },
            RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::PermissionDenied, .message = "Write only"}
        );
        queueUpload(*queue, writeLocalFile("uploaded.txt", content), target);

        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_EQ(fake_->contentOf(target), content);
    }

    TEST_F(ConnectionWaitTests, AnArchiveDownloadWaitsWithoutFailingUntilAConnectionIsFree)
    {
        auto queue = makeWakingQueue();
        const auto source = addRemoteFile("packed.txt");
        const auto archive = localDirectory_.path() / "packed.tar";
        fake_->takeAllConnections();
        const auto added = runOnStrand(
            [&]()
            {
                return queue->addArchiveDownloadOperation(
                    fake_,
                    Ids::generateOperationId(),
                    {SharedData::ArchiveDownloadRoot{
                        .path = source,
                        .entry = {.name = "packed.txt", .type = SharedData::FileType::Regular, .size = 8u * 1024u},
                    }},
                    toLocal(archive),
                    TarArchive::Compression::None,
                    {},
                    false
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();

        EXPECT_TRUE(workUntilWaiting(*queue)) << "the queue kept working an archive whose entry waits";
        EXPECT_TRUE(completions().empty()) << "the archive ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        EXPECT_GT(std::filesystem::file_size(archive), 8u * 1024u);
    }

    TEST_F(ConnectionWaitTests, AnArchiveUploadWaitsWithoutFailingUntilAConnectionIsFree)
    {
        auto queue = makeWakingQueue();
        fake_->addDirectory(remote("/home/test"));
        const auto target = remote("/home/test/packed.tar");
        const auto local = writeLocalFile("packed.txt", uploadedContent());
        fake_->takeAllConnections();
        const auto operationId = Ids::generateOperationId();
        std::shared_ptr<HeardProgress> heard{};
        const auto added = runOnStrand(
            [&]()
            {
                heard = observeProgress(operationId);
                return queue->addArchiveUploadOperation(
                    fake_, operationId, {local}, target, TarArchive::Compression::None, {}, false
                );
            }
        );
        ASSERT_TRUE(added.has_value()) << added.error().toString();

        EXPECT_TRUE(workUntilWaiting(*queue)) << "the queue kept working an archive upload that waits";
        EXPECT_TRUE(completions().empty()) << "the archive upload ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        ASSERT_NO_FATAL_FAILURE(expectCompleted());
        const auto packed = fake_->contentOf(target);
        ASSERT_TRUE(packed.has_value());
        EXPECT_GT(packed->size(), uploadedContent().size());
        EXPECT_NE(packed->find(uploadedContent()), std::string::npos);
        // Prepared twice, the source still counts once.
        runOnStrand(
            [&heard]()
            {
                ASSERT_FALSE(heard->progress.empty());
                for (auto const& progress : heard->progress)
                    EXPECT_EQ(std::get<SharedData::TransferProgress>(progress).max, uploadedContent().size());
            }
        );
    }

    TEST_F(ConnectionWaitTests, AScanReadsItsIgnoreFileWhileEveryConnectionIsTaken)
    {
        fake_->addFile(remote("/home/test/tree/.gitignore"), "skipped.txt\n");
        fake_->addFile(remote("/home/test/tree/kept.txt"), "kept");
        fake_->addFile(remote("/home/test/tree/skipped.txt"), "skipped");
        fake_->takeAllConnections();

        const auto names = scannedNames(remote("/home/test/tree"));

        EXPECT_TRUE(names.contains("kept.txt"));
        EXPECT_FALSE(names.contains("skipped.txt")) << "the scan did not read its ignore file";
    }

    TEST_F(ConnectionWaitTests, AScanAppliesAnIgnoreFileTooLargeOnlyUpToItsLastWholeLine)
    {
        const std::string rules = "skipped.txt\n";
        const std::string cutRule = "kernel.bin\n";
        std::string ignoreFile = rules;
        while (ignoreFile.size() < ScanOperation::maximumIgnoreFileSize - 2u)
            ignoreFile += "#\n";
        ignoreFile += cutRule;
        ASSERT_EQ(ignoreFile.substr(ScanOperation::maximumIgnoreFileSize - 2u, 2u), "ke");
        fake_->addFile(remote("/home/test/tree/.gitignore"), ignoreFile);
        fake_->addFile(remote("/home/test/tree/ke"), "ke");
        fake_->addFile(remote("/home/test/tree/skipped.txt"), "skipped");

        const auto names = scannedNames(remote("/home/test/tree"));

        EXPECT_TRUE(names.contains("ke")) << "the rule cut at the size limit was applied";
        EXPECT_FALSE(names.contains("skipped.txt")) << "the rules before the cut were dropped";
    }
}
