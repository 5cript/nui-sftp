#pragma once

#include "test_operation_queue_batches.hpp"

#include <persistence/state/sftp_options.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
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

        expectCompleted();
        EXPECT_EQ(completions().front().operationId, operationId);
        EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / "file.txt"));
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

        expectCompleted();
        EXPECT_EQ(fake_->contentOf(target), uploadedContent());
    }

    TEST_F(ConnectionWaitTests, AnUploadWhoseTailCheckFindsEveryConnectionTakenContinuesItsPart)
    {
        auto queue = makeWakingQueue();
        const auto target = remote("/home/test/uploaded.txt");
        const auto content = uploadedContent();
        fake_->addFile(
            remote(fmt::format("/home/test/uploaded.txt{}", Persistence::defaultTempFileSuffix)),
            content.substr(0, content.size() / 2)
        );
        // Starting over truncates the part.
        fake_->failWhen<RemoteIo::OpenFile>(
            [](RemoteIo::OpenFile const& open)
            {
                return RemoteIo::contains(open.mode, RemoteIo::OpenMode::Truncate);
            },
            RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::Failure, .message = "The upload started over"}
        );
        fake_->takeAllConnections();
        queueUpload(*queue, writeLocalFile("uploaded.txt", content), target);

        EXPECT_TRUE(workUntilWaiting(*queue));
        EXPECT_TRUE(completions().empty()) << "the upload ended while every connection was taken";
        EXPECT_EQ(fake_->busyOpens(), 1u);

        fake_->freeConnections();
        workUntilCompletions(1);

        expectCompleted();
        EXPECT_EQ(fake_->contentOf(target), content);
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
        EXPECT_LE(fake_->busyOpens(), names.size()) << "a waiting file opened again before it was woken";

        const auto wakesBefore = wakes_.load();
        fake_->freeConnections();
        EXPECT_GT(wakes_.load(), wakesBefore) << "freeing a connection did not wake the queue";
        workUntilCompletions(1);

        expectCompleted();
        EXPECT_TRUE(completions().front().failedEntries.empty());
        for (auto const& name : names)
            EXPECT_TRUE(std::filesystem::exists(localDirectory_.path() / name)) << name;
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

        expectCompleted();
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

        expectCompleted();
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
}
