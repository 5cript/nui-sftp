#pragma once

#include <backend/file_tracking/batching_remote.hpp>
#include "io_context_runs.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class BatchingRemoteTest : public ::testing::Test
    {
      protected:
        /**
         * @brief Records what reaches the server side, the test ends the operations.
         */
        class FakeInner : public FileTracking::BulkTrackerRemote
        {
          public:
            struct Queued
            {
                std::string kind;
                std::vector<std::string> paths;
                OnEnded onEnded{};
                OnEntryEnded onEntryEnded{};
                OnUploadEnded onUploadEnded{};
                OnUploadEntryEnded onUploadEntryEnded{};
                OnUploadProgress onUploadProgress{};
                OnUploadEntryProgress onUploadEntryProgress{};
            };

            std::expected<std::optional<FileTracking::ServerState>, std::string>
            stat(Paths::RemotePath const& path) override
            {
                queued.push_back({.kind = "stat", .paths = {path.value()}});
                return std::nullopt;
            }

            std::expected<std::optional<FileTracking::ServerState>, std::string>
            linkStat(Paths::RemotePath const& path) override
            {
                queued.push_back({.kind = "linkStat", .paths = {path.value()}});
                return std::nullopt;
            }

            std::expected<void, std::string> download(
                Paths::RemotePath const& source,
                Paths::LocalPath const& destination,
                std::uint64_t,
                OnEnded onEnded
            ) override
            {
                queued.push_back({
                    .kind = "download",
                    .paths = {source.value(), destination.toUtf8()},
                    .onEnded = std::move(onEnded),
                });
                return {};
            }

            std::expected<void, std::string> upload(
                FileTracking::UploadRequest const& request,
                OnUploadEnded onEnded,
                OnUploadProgress onProgress
            ) override
            {
                if (failQueueing)
                    return std::unexpected{std::string{"refused"}};
                queued.push_back({
                    .kind = "upload",
                    .paths = {request.destination.value()},
                    .onUploadEnded = std::move(onEnded),
                    .onUploadProgress = std::move(onProgress),
                });
                return {};
            }

            void cancelUpload(Paths::RemotePath const& destination, FileTracking::PartialFile partial) override
            {
                cancels.emplace_back(destination.value(), partial);
            }

            std::expected<void, std::string>
            discardPartialUpload(Paths::RemotePath const& destination, OnEnded onEnded) override
            {
                queued.push_back({.kind = "discard", .paths = {destination.value()}, .onEnded = std::move(onEnded)});
                return {};
            }

            std::expected<void, std::string>
            rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded) override
            {
                queued.push_back({
                    .kind = "rename",
                    .paths = {source.value(), destination.value()},
                    .onEnded = std::move(onEnded),
                });
                return {};
            }

            std::expected<void, std::string> remove(Paths::RemotePath const& path, OnEnded onEnded) override
            {
                queued.push_back({.kind = "remove", .paths = {path.value()}, .onEnded = std::move(onEnded)});
                return {};
            }

            std::expected<void, std::string> uploadAll(
                std::vector<FileTracking::UploadRequest> const& requests,
                OnUploadEntryEnded onEnded,
                OnUploadEntryProgress onProgress
            ) override
            {
                if (failQueueing)
                    return std::unexpected{std::string{"refused"}};
                Queued bulk{
                    .kind = "uploadAll",
                    .paths = {},
                    .onUploadEntryEnded = std::move(onEnded),
                    .onUploadEntryProgress = std::move(onProgress),
                };
                for (auto const& request : requests)
                    bulk.paths.push_back(request.destination.value());
                queued.push_back(std::move(bulk));
                return {};
            }

            std::expected<void, std::string>
            removeAll(std::vector<Paths::RemotePath> const& paths, OnEntryEnded onEnded) override
            {
                Queued bulk{.kind = "removeAll", .paths = {}, .onEnded = {}, .onEntryEnded = std::move(onEnded)};
                for (auto const& path : paths)
                    bulk.paths.push_back(path.value());
                queued.push_back(std::move(bulk));
                return {};
            }

            std::vector<Queued*> of(std::string const& kind)
            {
                std::vector<Queued*> found;
                for (auto& operation : queued)
                    if (operation.kind == kind)
                        found.push_back(&operation);
                return found;
            }

            std::vector<Queued> queued{};
            std::vector<std::pair<std::string, FileTracking::PartialFile>> cancels{};
            bool failQueueing{false};
        };

        boost::asio::io_context ioc_;
        boost::asio::strand<boost::asio::any_io_executor> strand_{ioc_.get_executor()};
        std::filesystem::path root_;
        FakeInner* inner_{nullptr};
        std::unique_ptr<FileTracking::BatchingRemote> remote_;
        /** @brief How each upload or delete of the test ended, by its name. */
        std::map<std::string, SharedData::OperationCompletionReason> ended_{};
        /** @brief What the server had at the destination of each upload of the test that ended, by its name. */
        std::map<std::string, std::optional<FileTracking::ServerState>> destinationStates_{};
        /** @brief Why each upload of the test that ended failed, by its name. */
        std::map<std::string, std::optional<FileTracking::UploadFailureInfo>> failures_{};
        /** @brief The progress each upload of the test heard, by its name. */
        std::map<std::string, std::vector<FileTracking::UploadProgress>> progress_{};

        void SetUp() override
        {
            root_ = std::filesystem::canonical(temporaryRoot) /
                ("batching_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root_);
            makeRemote(
                {.quiet = std::chrono::milliseconds{50}, .longest = std::chrono::milliseconds{400}, .maxEntries = 4}
            );
        }

        void TearDown() override
        {
            remote_.reset();
            if (!root_.empty())
                std::filesystem::remove_all(root_);
        }

        void makeRemote(FileTracking::BatchingRemote::Limits limits)
        {
            auto inner = std::make_unique<FakeInner>();
            inner_ = inner.get();
            remote_ = std::make_unique<FileTracking::BatchingRemote>(strand_, std::move(inner), limits);
        }

        /**
         * @brief A regular file @p name below the test directory.
         */
        Paths::LocalPath file(std::string const& name)
        {
            std::ofstream{root_ / name} << name;
            return Paths::LocalPath::make(root_ / name).value();
        }

        static Paths::RemotePath remote(std::string const& path)
        {
            return Paths::RemotePath::make(path).value();
        }

        FileTracking::TrackerRemote::OnEnded recordAs(std::string name)
        {
            return [this, name = std::move(name)](SharedData::OperationCompletionReason reason)
            {
                ended_[name] = reason;
            };
        }

        FileTracking::TrackerRemote::OnUploadEnded recordUploadAs(std::string name)
        {
            return [this, name = std::move(name)](FileTracking::UploadEnd const& ended)
            {
                ended_[name] = ended.reason;
                destinationStates_[name] = ended.destinationState;
                failures_[name] = ended.failure;
            };
        }

        FileTracking::TrackerRemote::OnUploadProgress recordProgressAs(std::string name)
        {
            return [this, name = std::move(name)](FileTracking::UploadProgress const& progress)
            {
                progress_[name].push_back(progress);
            };
        }

        void upload(std::string const& name, bool createMissingDirectories = true)
        {
            ASSERT_TRUE(remote_
                    ->upload(
                        FileTracking::UploadRequest{
                            .source = file(name),
                            .destination = remote("/srv/" + name),
                            .createMissingDirectories = createMissingDirectories,
                        },
                        recordUploadAs(name),
                        recordProgressAs(name)
                    )
                    .has_value());
        }

        void remove(std::string const& name)
        {
            ASSERT_TRUE(remote_->remove(remote("/srv/" + name), recordAs(name)).has_value());
        }

        void runFor(std::chrono::milliseconds duration)
        {
            ioc_.restart();
            ioc_.run_for(duration);
        }

        bool
        runUntil(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds{10})
        {
            return ::Test::runUntil(ioc_, condition, timeout);
        }
    };

    TEST_F(BatchingRemoteTest, AnUploadAloneGoesOnItsOwnOnceTheBatchIsQuiet)
    {
        upload("a.txt");

        EXPECT_TRUE(inner_->queued.empty());
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        ASSERT_EQ(inner_->of("upload").size(), 1u);
        inner_->of("upload").front()->onUploadEnded({.reason = SharedData::OperationCompletionReason::Completed});
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Completed);
    }

    TEST_F(BatchingRemoteTest, UploadsGatheredTogetherGoAsOneBulkAndEachHearsItsOwnEnd)
    {
        upload("a.txt");
        upload("b.txt");
        upload("c.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        const auto bulks = inner_->of("uploadAll");
        ASSERT_EQ(bulks.size(), 1u);
        EXPECT_EQ(bulks.front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/b.txt", "/srv/c.txt"}));
        EXPECT_TRUE(inner_->of("upload").empty());

        const FileTracking::ServerState uploaded{.mtime = 7u, .size = 5u};
        bulks.front()->onUploadEntryEnded(1, {.reason = SharedData::OperationCompletionReason::Failed});
        bulks.front()->onUploadEntryEnded(
            0, {.reason = SharedData::OperationCompletionReason::Completed, .destinationState = uploaded}
        );
        bulks.front()->onUploadEntryEnded(2, {.reason = SharedData::OperationCompletionReason::Canceled});
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(ended_.at("b.txt"), SharedData::OperationCompletionReason::Failed);
        EXPECT_EQ(ended_.at("c.txt"), SharedData::OperationCompletionReason::Canceled);
        EXPECT_EQ(destinationStates_.at("a.txt"), uploaded);
        EXPECT_FALSE(destinationStates_.at("b.txt").has_value());
    }

    TEST_F(BatchingRemoteTest, UploadsThatMustNotCreateDirectoriesOrAreNoFilesGoOnTheirOwn)
    {
        upload("a.txt");
        upload("b.txt");
        upload("item.txt", /*createMissingDirectories=*/false);
        std::filesystem::create_directory(root_ / "directory");
        ASSERT_TRUE(remote_
                ->upload(
                    FileTracking::UploadRequest{
                        .source = Paths::LocalPath::make(root_ / "directory").value(),
                        .destination = remote("/srv/directory"),
                        .createMissingDirectories = true,
                    },
                    recordUploadAs("directory"),
                    {}
                )
                .has_value());

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        ASSERT_EQ(inner_->of("uploadAll").size(), 1u);
        EXPECT_EQ(inner_->of("uploadAll").front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/b.txt"}));
        ASSERT_EQ(inner_->of("upload").size(), 2u);
        EXPECT_EQ(inner_->of("upload")[0]->paths.front(), "/srv/item.txt");
        EXPECT_EQ(inner_->of("upload")[1]->paths.front(), "/srv/directory");
    }

    TEST_F(BatchingRemoteTest, DeletesGatheredTogetherGoAsOneBulkAndAloneOnTheirOwn)
    {
        remove("a.txt");
        remove("b.txt");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        ASSERT_EQ(inner_->of("removeAll").size(), 1u);
        EXPECT_EQ(inner_->of("removeAll").front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/b.txt"}));
        inner_->of("removeAll").front()->onEntryEnded(0, SharedData::OperationCompletionReason::Completed);
        inner_->of("removeAll").front()->onEntryEnded(1, SharedData::OperationCompletionReason::Failed);
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(ended_.at("b.txt"), SharedData::OperationCompletionReason::Failed);

        remove("c.txt");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->of("remove").empty();
            }
        ));
        EXPECT_EQ(inner_->of("remove").front()->paths.front(), "/srv/c.txt");
        EXPECT_EQ(inner_->of("removeAll").size(), 1u);
    }

    TEST_F(BatchingRemoteTest, AFullBatchGoesAtOnceAndTheNextOneGathersAnew)
    {
        for (auto const* name : {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"})
            upload(name);

        // Only what is due now, the quiet time of the next batch has not passed.
        ioc_.restart();
        ioc_.poll();
        ASSERT_EQ(inner_->of("uploadAll").size(), 1u);
        EXPECT_EQ(inner_->of("uploadAll").front()->paths.size(), 4u);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->of("upload").empty();
            }
        ));
        EXPECT_EQ(inner_->of("upload").front()->paths.front(), "/srv/e.txt");
    }

    TEST_F(BatchingRemoteTest, ABatchThatKeepsGrowingGoesAfterTheLongestTime)
    {
        // Only the longest time can send it, the batch is never quiet that long.
        constexpr auto longest = std::chrono::milliseconds{300};
        makeRemote({.quiet = std::chrono::seconds{10}, .longest = longest, .maxEntries = 100});
        const auto start = std::chrono::steady_clock::now();
        std::size_t uploaded = 0;
        while (inner_->queued.empty() && std::chrono::steady_clock::now() - start < std::chrono::seconds{10})
        {
            upload("file" + std::to_string(uploaded++) + ".txt");
            runFor(std::chrono::milliseconds{60});
        }
        const auto waited = std::chrono::steady_clock::now() - start;

        EXPECT_GE(waited, longest);
        ASSERT_EQ(inner_->of("uploadAll").size(), 1u);
        EXPECT_EQ(inner_->of("uploadAll").front()->paths.size(), uploaded);
    }

    TEST_F(BatchingRemoteTest, ABulkTheQueueRefusesEndsEachOfItsUploadsAsFailed)
    {
        inner_->failQueueing = true;
        upload("a.txt");
        upload("b.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return ended_.size() == 2u;
            }
        ));
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Failed);
        EXPECT_EQ(ended_.at("b.txt"), SharedData::OperationCompletionReason::Failed);
        for (auto const* name : {"a.txt", "b.txt"})
        {
            ASSERT_TRUE(failures_.at(name).has_value()) << name;
            EXPECT_EQ(failures_.at(name)->reason, SharedData::UploadFailureReason::NotQueued) << name;
            EXPECT_EQ(failures_.at(name)->detail, "refused") << name;
        }
    }

    TEST_F(BatchingRemoteTest, AnUploadTheQueueRefusesEndsFailedAsNotQueuedAndTellsWhy)
    {
        inner_->failQueueing = true;
        upload("a.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return ended_.contains("a.txt");
            }
        ));
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Failed);
        ASSERT_TRUE(failures_.at("a.txt").has_value());
        EXPECT_EQ(failures_.at("a.txt")->reason, SharedData::UploadFailureReason::NotQueued);
        EXPECT_EQ(failures_.at("a.txt")->detail, "refused");
        EXPECT_FALSE(progress_.contains("a.txt"));
    }

    TEST_F(BatchingRemoteTest, ARefusedUploadWhoseSourceIsGoneEndsCanceledWithoutAFailure)
    {
        inner_->failQueueing = true;
        upload("a.txt");
        upload("b.txt");
        std::filesystem::remove(root_ / "a.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return ended_.size() == 2u;
            }
        ));
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Canceled);
        EXPECT_FALSE(failures_.at("a.txt").has_value());
        EXPECT_EQ(ended_.at("b.txt"), SharedData::OperationCompletionReason::Failed);
        ASSERT_TRUE(failures_.at("b.txt").has_value());
        EXPECT_EQ(failures_.at("b.txt")->reason, SharedData::UploadFailureReason::NotQueued);
    }

    TEST_F(BatchingRemoteTest, EverythingElsePassesThroughAtOnce)
    {
        ASSERT_TRUE(remote_->rename(remote("/srv/a"), remote("/srv/b"), recordAs("rename")).has_value());
        std::ignore = remote_->stat(remote("/srv/a"));
        std::ignore = remote_->linkStat(remote("/srv/a"));
        ASSERT_TRUE(remote_->download(remote("/srv/a"), file("a"), 1u, recordAs("download")).has_value());

        ASSERT_EQ(inner_->queued.size(), 4u);
        EXPECT_EQ(inner_->queued[0].kind, "rename");
        EXPECT_EQ(inner_->queued[1].kind, "stat");
        EXPECT_EQ(inner_->queued[2].kind, "linkStat");
        EXPECT_EQ(inner_->queued[3].kind, "download");
    }

    TEST_F(BatchingRemoteTest, AGatheredUploadStoppedBeforeItsBatchGoesEndsCanceledAndIsNotSent)
    {
        upload("a.txt");
        upload("b.txt");
        remote_->cancelUpload(remote("/srv/a.txt"), FileTracking::PartialFile::Kept);

        EXPECT_TRUE(ended_.empty());
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Canceled);
        EXPECT_TRUE(inner_->of("uploadAll").empty());
        ASSERT_EQ(inner_->of("upload").size(), 1u);
        EXPECT_EQ(inner_->of("upload").front()->paths.front(), "/srv/b.txt");
        EXPECT_TRUE(inner_->cancels.empty());
    }

    TEST_F(BatchingRemoteTest, AStopOfASentUploadAndADiscardReachTheInnerRemote)
    {
        upload("a.txt");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));

        remote_->cancelUpload(remote("/srv/a.txt"), FileTracking::PartialFile::Discarded);
        ASSERT_TRUE(remote_->discardPartialUpload(remote("/srv/b.txt"), recordAs("discard")).has_value());

        ASSERT_EQ(inner_->cancels.size(), 1u);
        EXPECT_EQ(inner_->cancels.front().first, "/srv/a.txt");
        EXPECT_EQ(inner_->cancels.front().second, FileTracking::PartialFile::Discarded);
        ASSERT_EQ(inner_->of("discard").size(), 1u);
        EXPECT_EQ(inner_->of("discard").front()->paths.front(), "/srv/b.txt");
        inner_->of("discard").front()->onEnded(SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(ended_.at("discard"), SharedData::OperationCompletionReason::Completed);
        EXPECT_FALSE(ended_.contains("a.txt"));
    }

    TEST_F(BatchingRemoteTest, LargeFilesAndContinuingUploadsGoOnTheirOwn)
    {
        makeRemote(
            {.quiet = std::chrono::milliseconds{50},
                .longest = std::chrono::milliseconds{400},
                .maxEntries = 10,
                .aloneFromBytes = 8u}
        );
        upload("a.txt");
        upload("b.txt");
        upload("large-file.txt");
        ASSERT_TRUE(remote_
                ->upload(
                    FileTracking::UploadRequest{
                        .source = file("c.txt"),
                        .destination = remote("/srv/renamed.txt"),
                        .createMissingDirectories = true,
                        .continuesPartialOf = remote("/srv/c.txt"),
                    },
                    recordUploadAs("renamed.txt"),
                    {}
                )
                .has_value());

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        ASSERT_EQ(inner_->of("uploadAll").size(), 1u);
        EXPECT_EQ(inner_->of("uploadAll").front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/b.txt"}));
        ASSERT_EQ(inner_->of("upload").size(), 2u);
        EXPECT_EQ(inner_->of("upload")[0]->paths.front(), "/srv/large-file.txt");
        EXPECT_EQ(inner_->of("upload")[1]->paths.front(), "/srv/renamed.txt");
    }

    TEST_F(BatchingRemoteTest, AnUploadSentOnItsOwnHearsWhatTheInnerRemoteTellsOfIt)
    {
        upload("a.txt");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        ASSERT_EQ(inner_->of("upload").size(), 1u);
        auto const* sent = inner_->of("upload").front();
        ASSERT_TRUE(sent->onUploadProgress);

        sent->onUploadProgress({.bytesSent = 3u, .bytesTotal = 5u, .filesDone = 0u, .fileCount = 1u});

        ASSERT_EQ(progress_["a.txt"].size(), 1u);
        EXPECT_EQ(progress_["a.txt"].front().bytesSent, 3u);
        EXPECT_EQ(progress_["a.txt"].front().bytesTotal, 5u);
    }

    TEST_F(BatchingRemoteTest, EachUploadOfABulkHearsTheProgressAtTheIndexOfItsRequest)
    {
        upload("a.txt");
        upload("b.txt");
        upload("c.txt");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        const auto bulks = inner_->of("uploadAll");
        ASSERT_EQ(bulks.size(), 1u);
        ASSERT_EQ(bulks.front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/b.txt", "/srv/c.txt"}));
        ASSERT_TRUE(bulks.front()->onUploadEntryProgress);

        bulks.front()->onUploadEntryProgress(2, {.bytesSent = 2u, .bytesTotal = 5u});
        bulks.front()->onUploadEntryProgress(0, {.bytesSent = 1u, .bytesTotal = 5u});
        bulks.front()->onUploadEntryProgress(2, {.bytesSent = 5u, .bytesTotal = 5u, .transferred = true});
        bulks.front()->onUploadEntryProgress(3, {.bytesSent = 9u, .bytesTotal = 9u});

        ASSERT_EQ(progress_["a.txt"].size(), 1u);
        EXPECT_EQ(progress_["a.txt"].front().bytesSent, 1u);
        EXPECT_FALSE(progress_.contains("b.txt"));
        ASSERT_EQ(progress_["c.txt"].size(), 2u);
        EXPECT_EQ(progress_["c.txt"][0].bytesSent, 2u);
        EXPECT_TRUE(progress_["c.txt"][1].transferred);
    }

    TEST_F(BatchingRemoteTest, AGatheredUploadStoppedBeforeItsBatchGoesHearsNothingMore)
    {
        upload("a.txt");
        upload("b.txt");
        upload("c.txt");
        remote_->cancelUpload(remote("/srv/b.txt"), FileTracking::PartialFile::Kept);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !inner_->queued.empty();
            }
        ));
        const auto bulks = inner_->of("uploadAll");
        ASSERT_EQ(bulks.size(), 1u);
        ASSERT_EQ(bulks.front()->paths, (std::vector<std::string>{"/srv/a.txt", "/srv/c.txt"}));

        bulks.front()->onUploadEntryProgress(0, {.bytesSent = 1u, .bytesTotal = 5u});
        bulks.front()->onUploadEntryProgress(1, {.bytesSent = 4u, .bytesTotal = 5u});
        bulks.front()->onUploadEntryEnded(0, {.reason = SharedData::OperationCompletionReason::Completed});
        bulks.front()->onUploadEntryEnded(1, {.reason = SharedData::OperationCompletionReason::Completed});

        EXPECT_EQ(ended_.at("b.txt"), SharedData::OperationCompletionReason::Canceled);
        EXPECT_FALSE(progress_.contains("b.txt"));
        ASSERT_EQ(progress_["a.txt"].size(), 1u);
        EXPECT_EQ(progress_["a.txt"].front().bytesSent, 1u);
        ASSERT_EQ(progress_["c.txt"].size(), 1u);
        EXPECT_EQ(progress_["c.txt"].front().bytesSent, 4u);
        EXPECT_EQ(ended_.at("a.txt"), SharedData::OperationCompletionReason::Completed);
        EXPECT_EQ(ended_.at("c.txt"), SharedData::OperationCompletionReason::Completed);
    }

    TEST_F(BatchingRemoteTest, NothingGatheredIsSentOnceTheRemoteIsGone)
    {
        upload("a.txt");
        upload("b.txt");
        remote_.reset();
        inner_ = nullptr;

        ASSERT_TRUE(::Test::runUntilIdle(ioc_));

        EXPECT_TRUE(ended_.empty());
    }
}
