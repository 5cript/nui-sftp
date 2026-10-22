#pragma once

#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <backend/file_tracking/tracker.hpp>
#include <utility/localized_message.hpp>
#include <paths/native.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern std::filesystem::path temporaryRoot;

namespace Test
{
    class TrackerTest : public ::testing::Test
    {
      protected:
        /**
         * @brief Records what the tracker queues, the test ends the operations.
         */
        class FakeRemote : public FileTracking::TrackerRemote
        {
          public:
            struct Queued
            {
                std::string kind;
                std::string source;
                std::string destination;
                FileTracking::UploadRequest upload{};
                OnEnded onEnded{};
                OnUploadEnded onUploadEnded{};
                bool ended{false};
            };

            std::expected<std::optional<FileTracking::ServerState>, std::string>
            stat(Paths::RemotePath const& path) override
            {
                ++stats;
                const auto found = server.find(path.value());
                if (found == server.end())
                    return std::nullopt;
                return found->second;
            }

            std::expected<std::optional<FileTracking::ServerState>, std::string>
            linkStat(Paths::RemotePath const& path) override
            {
                return stat(path);
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
                    .source = source.value(),
                    .destination = destination.toUtf8(),
                    .onEnded = std::move(onEnded),
                });
                return {};
            }

            std::expected<void, std::string>
            upload(FileTracking::UploadRequest const& request, OnUploadEnded onEnded) override
            {
                queued.push_back({
                    .kind = "upload",
                    .source = request.source.toUtf8(),
                    .destination = request.destination.value(),
                    .upload = request,
                    .onEnded = {},
                    .onUploadEnded = std::move(onEnded),
                });
                return {};
            }

            std::expected<void, std::string>
            rename(Paths::RemotePath const& source, Paths::RemotePath const& destination, OnEnded onEnded) override
            {
                queued.push_back({
                    .kind = "rename",
                    .source = source.value(),
                    .destination = destination.value(),
                    .upload = {},
                    .onEnded = std::move(onEnded),
                });
                return {};
            }

            std::expected<void, std::string> remove(Paths::RemotePath const& path, OnEnded onEnded) override
            {
                queued.push_back({
                    .kind = "remove",
                    .source = path.value(),
                    .destination = {},
                    .upload = {},
                    .onEnded = std::move(onEnded),
                });
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

            /**
             * @brief The sources of the operations of @p kind, in the order they were queued.
             */
            std::vector<std::string> sourcesOf(std::string const& kind)
            {
                std::vector<std::string> sources;
                for (auto const* operation : of(kind))
                    sources.push_back(operation->source);
                return sources;
            }

            std::map<std::string, FileTracking::ServerState> server{};
            std::vector<Queued> queued{};
            std::size_t stats{0};
        };

        class FakeView : public FileTracking::TrackerView
        {
          public:
            void showEntry(SharedData::TrackedEntry const& entry) override
            {
                entries.insert_or_assign(entry.watchId.value(), entry);
            }

            void removeEntry(Ids::WatchId const& watchId) override
            {
                entries.erase(watchId.value());
                removed.push_back(watchId.value());
            }

            void ask(SharedData::ServerChangedQuestion const& question) override
            {
                questions.push_back(question);
            }

            void showOrphanedCount(std::size_t count) override
            {
                orphanedCounts.push_back(count);
            }

            void showProgress(SharedData::TrackerProgress const& shown) override
            {
                progress.push_back(shown);
            }

            std::map<std::string, SharedData::TrackedEntry> entries{};
            std::vector<std::string> removed{};
            std::vector<SharedData::ServerChangedQuestion> questions{};
            std::vector<std::size_t> orphanedCounts{};
            std::vector<SharedData::TrackerProgress> progress{};
        };

        using Opened = std::expected<FileTracking::Tracker::OpenedCopy, FileTracking::Tracker::OpenFailure>;

        boost::asio::io_context ioc_;
        boost::asio::strand<boost::asio::any_io_executor> strand_{ioc_.get_executor()};
        std::filesystem::path root_;
        std::unique_ptr<FileTracking::TempDirInstanceManager> instances_;
        std::unique_ptr<FileTracking::LocalWatchManager> localWatches_;
        SharedData::WatchOptions defaults_{
            .autoReupload = true,
            .moveRemoteOnLocalMove = true,
            .deleteRemoteOnLocalDelete = true,
            .checkRemoteChanges = true,
        };
        FakeRemote* remote_{nullptr};
        FakeView* view_{nullptr};
        std::unique_ptr<FileTracking::Tracker> tracker_;

        void SetUp() override
        {
            root_ = std::filesystem::canonical(temporaryRoot) /
                ("tracker_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root_ / "downloads");
            std::filesystem::create_directories(root_ / "local" / "project" / "src");
            write(root_ / "local" / "notes.txt", "notes");
            write(root_ / "local" / "project" / "src" / "main.cpp", "int main() {}");
            instances_ = std::make_unique<FileTracking::TempDirInstanceManager>(strand_, root_ / "downloads");
            localWatches_ = std::make_unique<FileTracking::LocalWatchManager>(strand_, root_ / "downloads");
            auto remote = std::make_unique<FakeRemote>();
            auto view = std::make_unique<FakeView>();
            remote_ = remote.get();
            view_ = view.get();
            tracker_ = std::make_unique<FileTracking::Tracker>(
                strand_,
                *instances_,
                *localWatches_,
                std::move(remote),
                std::move(view),
                std::string{Persistence::defaultTempFileSuffix},
                [this]()
                {
                    return defaults_;
                }
            );
        }

        void TearDown() override
        {
            tracker_.reset();
            localWatches_.reset();
            instances_.reset();
            if (!root_.empty())
                std::filesystem::remove_all(root_);
        }

        static void write(std::filesystem::path const& path, std::string const& content)
        {
            std::ofstream{path, std::ios::binary | std::ios::trunc} << content;
        }

        static Paths::RemotePath remote(std::string const& path)
        {
            return Paths::RemotePath::make(path).value();
        }

        static Paths::LocalPath local(std::filesystem::path const& path)
        {
            return Paths::LocalPath::make(path).value();
        }

        static FileTracking::ServerState stateOf(std::filesystem::path const& path)
        {
            const auto states = FileTracking::itemStatesOf(path);
            return FileTracking::ServerState{.mtime = states.at(0).mtime, .size = states.at(0).size};
        }

        /**
         * @brief The files an upload of @p source sends as the operation tells of them: the regular files at and
         *        below it, in the state they have now.
         */
        static std::vector<FileTracking::SentFile> sentFilesOf(std::filesystem::path const& source)
        {
            std::vector<FileTracking::SentFile> sent;
            for (auto const& state : FileTracking::itemStatesOf(source))
            {
                if (const auto current = FileTracking::sourceStateOf(state.path))
                    sent.push_back({.path = local(state.path), .state = *current});
            }
            return sent;
        }

        /**
         * @brief Runs the handlers until @p condition holds, at most for @p timeout.
         */
        bool
        runUntil(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds{5})
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                ioc_.restart();
                ioc_.run_for(std::chrono::milliseconds{20});
                if (condition())
                    return true;
            }
            return false;
        }

        void runFor(std::chrono::milliseconds duration)
        {
            runUntil(
                []()
                {
                    return false;
                },
                duration
            );
        }

        /**
         * @brief Opens @p remotePath, the download brings @p content.
         */
        Opened open(std::string const& remotePath, bool watch, std::string const& content = "server content")
        {
            std::optional<Opened> opened;
            tracker_->open(
                remote(remotePath),
                content.size(),
                watch,
                [&opened](Opened const& result)
                {
                    opened = result;
                }
            );
            if (opened)
                return *opened;
            auto* download = remote_->of("download").back();
            write(download->destination, content);
            remote_->server[remotePath] = stateOf(download->destination);
            end(*download, SharedData::OperationCompletionReason::Completed);
            EXPECT_TRUE(opened.has_value());
            return opened.value_or(std::unexpected{FileTracking::Tracker::OpenFailure{"not answered"}});
        }

        /**
         * @brief Ends @p operation. Its callback stays, so nothing may depend on the queue dropping it. An upload that
         *        completes fails instead when the server no longer has what it expects, else it tells what the
         *        server has at its destination then.
         */
        void end(FakeRemote::Queued& operation, SharedData::OperationCompletionReason reason)
        {
            operation.ended = true;
            if (operation.onEnded)
                operation.onEnded(reason);
            if (!operation.onUploadEnded)
                return;
            if (reason != SharedData::OperationCompletionReason::Completed)
                return operation.onUploadEnded({.reason = reason});
            if (!holdsExpected(operation))
                return operation.onUploadEnded(
                    {.reason = SharedData::OperationCompletionReason::Failed, .changedOnServer = true}
                );
            operation.onUploadEnded(
                {.reason = SharedData::OperationCompletionReason::Completed,
                    .destinationState = serverStateAt(operation.destination),
                    .sentFiles = sentFilesOf(operation.source)}
            );
        }

        /**
         * @brief Completes @p operation, an upload whose compare passed: the server has @p state at its destination.
         */
        void completeWith(FakeRemote::Queued& operation, FileTracking::ServerState state)
        {
            remote_->server[operation.destination] = state;
            operation.ended = true;
            operation.onUploadEnded(
                {.reason = SharedData::OperationCompletionReason::Completed,
                    .destinationState = state,
                    .sentFiles = sentFilesOf(operation.source)}
            );
        }

        std::optional<FileTracking::ServerState> serverStateAt(std::string const& path) const
        {
            const auto found = remote_->server.find(path);
            if (found == remote_->server.end())
                return std::nullopt;
            return found->second;
        }

        /**
         * @brief Whether the server still has what @p upload expects at its destination, as its compare sees it: a
         *        known file that is gone changed too.
         */
        bool holdsExpected(FakeRemote::Queued const& upload) const
        {
            if (!upload.upload.expectedDestination)
                return true;
            return serverStateAt(upload.destination) == upload.upload.expectedDestination->file;
        }

        /**
         * @brief Ends every upload on its way as the server would, it has the local files then.
         */
        void completeUploads()
        {
            for (std::size_t index = 0; index < remote_->queued.size(); ++index)
            {
                auto& operation = remote_->queued[index];
                if (operation.kind != "upload" || operation.ended)
                    continue;
                // The compare comes before the server's copy is replaced.
                if (!holdsExpected(operation))
                {
                    end(operation, SharedData::OperationCompletionReason::Completed);
                    continue;
                }
                const auto destination = operation.destination;
                const auto source = operation.source;
                serve(source, destination);
                auto& served = remote_->queued[index];
                served.ended = true;
                served.onUploadEnded(
                    {.reason = SharedData::OperationCompletionReason::Completed,
                        .destinationState = serverStateAt(destination),
                        .sentFiles = sentFilesOf(source)}
                );
            }
        }

        /**
         * @brief The server gets @p source at @p destination as an upload brings it: files with their state,
         *        directories with their content and symbolic links as links.
         */
        void serve(std::filesystem::path const& source, std::string const& destination)
        {
            if (std::filesystem::is_symlink(source))
                remote_->server[destination] = FileTracking::ServerState{};
            else if (std::filesystem::is_regular_file(source))
                remote_->server[destination] = stateOf(source);
            else if (std::filesystem::is_directory(source))
            {
                remote_->server[destination] = FileTracking::ServerState{};
                for (auto const& entry : std::filesystem::directory_iterator{source})
                    serve(entry.path(), destination + "/" + entry.path().filename().generic_string());
            }
        }

        /**
         * @brief Ends every delete and rename on its way as the server would.
         */
        void completeRemovesAndRenames()
        {
            for (std::size_t index = 0; index < remote_->queued.size(); ++index)
            {
                auto& operation = remote_->queued[index];
                if ((operation.kind != "remove" && operation.kind != "rename") || operation.ended)
                    continue;
                if (const auto found = remote_->server.find(operation.source); found != remote_->server.end())
                {
                    if (operation.kind == "rename")
                        remote_->server[operation.destination] = found->second;
                    remote_->server.erase(operation.source);
                }
                end(remote_->queued[index], SharedData::OperationCompletionReason::Completed);
            }
        }

        /**
         * @brief Ends what is on its way and what starts after it, until nothing is left.
         */
        void completeEverything()
        {
            const auto onItsWay = [this]()
            {
                return std::ranges::any_of(
                    remote_->queued,
                    [](FakeRemote::Queued const& operation)
                    {
                        return operation.kind != "download" && !operation.ended;
                    }
                );
            };
            for (int round = 0; round < 100; ++round)
            {
                completeUploads();
                completeRemovesAndRenames();
                runFor(std::chrono::milliseconds{100});
                if (!onItsWay())
                    return;
            }
            ADD_FAILURE() << "operations kept coming";
        }

        std::size_t uploadsTo(std::string const& destination)
        {
            std::size_t count = 0;
            for (auto const* upload : remote_->of("upload"))
                count += upload->destination == destination ? 1u : 0u;
            return count;
        }

        SharedData::TrackedEntry const& onlyEntry()
        {
            EXPECT_EQ(view_->entries.size(), 1u);
            return view_->entries.begin()->second;
        }

        /**
         * @brief The progress the view showed last, nothing on its way before it showed any.
         */
        SharedData::TrackerProgress lastProgress() const
        {
            return view_->progress.empty() ? SharedData::TrackerProgress{} : view_->progress.back();
        }

        /**
         * @brief Runs the handlers until the view shows @p expected.
         */
        bool runUntilProgress(SharedData::TrackerProgress const& expected)
        {
            return runUntil(
                [this, &expected]()
                {
                    return lastProgress() == expected;
                }
            );
        }
    };

    TEST_F(TrackerTest, OpeningWithoutWatchingDownloadsIntoAnInstanceAndTracksNothing)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/false);

        ASSERT_TRUE(opened.has_value());
        EXPECT_EQ(opened->localPath.native().filename(), "notes.txt");
        EXPECT_EQ(opened->localPath.native().parent_path().parent_path(), root_ / "downloads");
        EXPECT_NE(instances_->findInstance(opened->instanceId.value()), nullptr);
        EXPECT_TRUE(view_->entries.empty());
    }

    TEST_F(TrackerTest, OpenAndWatchShowsAnEntryOnceTheDownloadIsThere)
    {
        defaults_.checkRemoteChanges = false;

        const auto opened = open("/srv/notes.txt", /*watch=*/true);

        ASSERT_TRUE(opened.has_value());
        auto const& entry = onlyEntry();
        EXPECT_EQ(entry.name, "notes.txt");
        EXPECT_EQ(entry.remotePath.value(), "/srv/notes.txt");
        EXPECT_EQ(
            entry.options,
            (SharedData::WatchOptions{
                .autoReupload = true,
                .moveRemoteOnLocalMove = false,
                .deleteRemoteOnLocalDelete = false,
                .checkRemoteChanges = false,
            })
        );
        EXPECT_TRUE(entry.downloadedCopy);
        EXPECT_FALSE(entry.uploading);
    }

    TEST_F(TrackerTest, AnUploadedOriginalStartsWithTheDefaultOptions)
    {
        defaults_.checkRemoteChanges = false;

        ASSERT_TRUE(
            tracker_->uploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv/notes.txt")).has_value()
        );

        EXPECT_EQ(onlyEntry().options, defaults_);
        EXPECT_FALSE(onlyEntry().downloadedCopy);
    }

    TEST_F(TrackerTest, AWatchOverlappingAnotherOnTheServerIsRefusedBeforeItDownloads)
    {
        std::optional<Opened> first;
        tracker_->open(
            remote("/srv/project"),
            0,
            true,
            [&first](Opened const& opened)
            {
                first = opened;
            }
        );
        const auto below = open("/srv/project/main.cpp", /*watch=*/true);
        const auto unwatched = open("/srv/project/main.cpp", /*watch=*/false);

        ASSERT_FALSE(below.has_value());
        ASSERT_TRUE(below.error().has_value());
        EXPECT_EQ(Utility::parseLocalizedMessage(*below.error()).key, "backend.fileTracking.remoteOverlaps");
        EXPECT_TRUE(unwatched.has_value());
        EXPECT_EQ(remote_->of("download").size(), 2u);
        EXPECT_FALSE(first.has_value());
    }

    TEST_F(TrackerTest, AFailedDownloadEndsItsInstanceAndFreesTheRemotePath)
    {
        std::optional<Opened> opened;
        tracker_->open(
            remote("/srv/notes.txt"),
            0,
            true,
            [&opened](Opened const& result)
            {
                opened = result;
            }
        );
        auto* download = remote_->of("download").back();
        const auto instanceDirectory = std::filesystem::path{download->destination}.parent_path();
        end(*download, SharedData::OperationCompletionReason::Failed);

        ASSERT_TRUE(opened.has_value());
        ASSERT_FALSE(opened->has_value());
        EXPECT_FALSE(opened->error().has_value());
        EXPECT_FALSE(std::filesystem::exists(instanceDirectory));
        EXPECT_TRUE(open("/srv/notes.txt", /*watch=*/true).has_value());
    }

    TEST_F(TrackerTest, ASavedChangeGoesBackToTheServer)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        write(opened->localPath.native(), "changed locally");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        EXPECT_TRUE(view_->questions.empty());
        EXPECT_TRUE(onlyEntry().uploading);
        const auto& expected = remote_->of("upload").back()->upload.expectedDestination;
        ASSERT_TRUE(expected.has_value());
        EXPECT_EQ(expected->file, remote_->server.at("/srv/notes.txt"));
        completeUploads();
        EXPECT_FALSE(onlyEntry().uploading);
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(opened->localPath.native()));
        // The upload compares the server's copy itself, and its end tells what the server has now.
        EXPECT_EQ(remote_->stats, 0u);
    }

    TEST_F(TrackerTest, TheNextUploadExpectsWhatTheLastOneSaidTheServerHas)
    {
        const auto notes = root_ / "local" / "notes.txt";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(notes), remote("/srv/notes.txt")).has_value());
        ASSERT_EQ(remote_->of("upload").size(), 1u);
        EXPECT_FALSE(remote_->of("upload").back()->upload.expectedDestination.has_value());
        // A server that keeps its own modification time.
        const FileTracking::ServerState kept{.mtime = 42u, .size = 5u};
        completeWith(*remote_->of("upload").back(), kept);

        write(notes, "changed locally");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 2u;
            }
        ));
        const auto& expected = remote_->of("upload").back()->upload.expectedDestination;
        ASSERT_TRUE(expected.has_value());
        EXPECT_EQ(expected->file, kept);
        completeUploads();
        runFor(std::chrono::milliseconds{200});
        EXPECT_TRUE(view_->questions.empty());
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(notes));
        EXPECT_EQ(remote_->stats, 0u);
    }

    TEST_F(TrackerTest, AServerCopyChangedMeanwhileIsAskedAboutOnceTheUploadFoundIt)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        remote_->server["/srv/notes.txt"].mtime += 100u;
        const auto changedOnServer = remote_->server.at("/srv/notes.txt");

        write(opened->localPath.native(), "changed locally");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        EXPECT_TRUE(view_->questions.empty());
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->questions.empty();
            }
        ));
        EXPECT_EQ(view_->questions.back().remotePath.value(), "/srv/notes.txt");
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), changedOnServer);
        EXPECT_TRUE(onlyEntry().uploading);
        tracker_->answer(view_->questions.back().questionId, SharedData::ServerChangedAnswer::Keep);
        runFor(std::chrono::milliseconds{200});
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_FALSE(onlyEntry().uploading);
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), changedOnServer);

        write(opened->localPath.native(), "changed again");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 2u;
            }
        ));
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return view_->questions.size() == 2u;
            }
        ));
        tracker_->answer(view_->questions.back().questionId, SharedData::ServerChangedAnswer::Replace);
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 3u);
        EXPECT_FALSE(remote_->of("upload").back()->upload.expectedDestination.has_value());
        completeUploads();
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(opened->localPath.native()));
        EXPECT_FALSE(onlyEntry().uploading);
        EXPECT_EQ(remote_->stats, 0u);
    }

    TEST_F(TrackerTest, AFileTheServerLostIsAskedAboutBeforeItIsUploadedAgain)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        remote_->server.erase("/srv/notes.txt");

        write(opened->localPath.native(), "changed locally");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->questions.empty();
            }
        ));
        EXPECT_FALSE(remote_->server.contains("/srv/notes.txt"));
        tracker_->answer(view_->questions.back().questionId, SharedData::ServerChangedAnswer::Replace);
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 2u);
        completeUploads();
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(opened->localPath.native()));
    }

    TEST_F(TrackerTest, AFileThatAppearedOnTheServerBeforeItsFirstUploadIsAskedAbout)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !onlyEntry().uploading;
            }
        ));

        write(root_ / "local" / "project" / "src" / "new.cpp", "new");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/new.cpp") == 1u;
            }
        ));
        const auto& expected = remote_->of("upload").back()->upload.expectedDestination;
        ASSERT_TRUE(expected.has_value());
        EXPECT_FALSE(expected->file.has_value());
        remote_->server["/srv/project/src/new.cpp"] = {.mtime = 7u, .size = 9u};
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->questions.empty();
            }
        ));
        EXPECT_EQ(view_->questions.back().remotePath.value(), "/srv/project/src/new.cpp");
        EXPECT_EQ(remote_->server.at("/srv/project/src/new.cpp"), (FileTracking::ServerState{.mtime = 7u, .size = 9u}));
    }

    TEST_F(TrackerTest, TheProgressShowsAChangeOnItsWayUntilItsUploadEnded)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        write(opened->localPath.native(), "changed locally");

        ASSERT_TRUE(runUntilProgress({.pending = 1u, .done = 0u, .waitingForAnswer = 0u}));
        completeUploads();
        ASSERT_TRUE(runUntilProgress({.pending = 0u, .done = 1u, .waitingForAnswer = 0u}));
        const auto shown = view_->progress.size();
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(view_->progress.size(), shown);
    }

    TEST_F(TrackerTest, TheProgressShowsAChangeWaitingForAnAnswer)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        remote_->server["/srv/notes.txt"].mtime += 100u;

        write(opened->localPath.native(), "changed locally");

        ASSERT_TRUE(runUntilProgress({.pending = 1u, .done = 0u, .waitingForAnswer = 0u}));
        completeUploads();
        ASSERT_TRUE(runUntilProgress({.pending = 1u, .done = 0u, .waitingForAnswer = 1u}));
        tracker_->answer(view_->questions.back().questionId, SharedData::ServerChangedAnswer::Keep);
        ASSERT_TRUE(runUntilProgress({.pending = 0u, .done = 1u, .waitingForAnswer = 0u}));
    }

    TEST_F(TrackerTest, TheProgressCountsADeleteOnTheServerUntilItEnded)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->progress.empty() && lastProgress().pending == 0u;
            }
        ));

        std::filesystem::remove(root_ / "local" / "project" / "src" / "main.cpp");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty() && lastProgress().pending > 0u;
            }
        ));
        end(*remote_->of("remove").front(), SharedData::OperationCompletionReason::Completed);
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return lastProgress().pending == 0u;
            }
        ));
        EXPECT_GE(lastProgress().done, 1u);
    }

    TEST_F(TrackerTest, AnAnswerToAnotherQuestionChangesNothing)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        remote_->server["/srv/notes.txt"].size += 1u;

        write(opened->localPath.native(), "changed locally");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->questions.empty();
            }
        ));
        tracker_->answer(Ids::generateQuestionId(), SharedData::ServerChangedAnswer::Replace);

        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_TRUE(onlyEntry().uploading);
    }

    TEST_F(TrackerTest, UploadAndWatchUploadsTheCanonicalItemFirst)
    {
        std::error_code linkError;
        std::filesystem::create_symlink(root_ / "local" / "notes.txt", root_ / "local" / "link.txt", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();

        const auto target = tracker_->checkUploadAndWatch(local(root_ / "local" / "link.txt"), remote("/srv"));
        ASSERT_TRUE(target.has_value()) << target.error();
        EXPECT_EQ(target->item.native(), root_ / "local" / "notes.txt");
        EXPECT_EQ(target->destination.value(), "/srv/notes.txt");

        ASSERT_TRUE(tracker_->uploadAndWatch(target->item, target->destination).has_value());

        const auto uploads = remote_->of("upload");
        ASSERT_EQ(uploads.size(), 1u);
        EXPECT_TRUE(uploads.front()->upload.refreshListing);
        EXPECT_FALSE(uploads.front()->upload.createMissingDirectories);
        EXPECT_EQ(remote_->stats, 0u);
        EXPECT_EQ(onlyEntry().remotePath.value(), "/srv/notes.txt");
        EXPECT_TRUE(onlyEntry().uploading);
    }

    TEST_F(TrackerTest, UploadAndWatchRefusesOverlapsLocallyAndOnTheServer)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());

        const auto sameItem = tracker_->checkUploadAndWatch(local(root_ / "local" / "project" / "src"), remote("/srv"));
        const auto sameRemote =
            tracker_->uploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv/project/notes.txt"));

        ASSERT_FALSE(sameItem.has_value());
        EXPECT_EQ(Utility::parseLocalizedMessage(sameItem.error()).key, "backend.fileTracking.watchOverlaps");
        ASSERT_FALSE(sameRemote.has_value());
        EXPECT_EQ(Utility::parseLocalizedMessage(sameRemote.error()).key, "backend.fileTracking.remoteOverlaps");
        EXPECT_EQ(view_->entries.size(), 1u);
    }

    TEST_F(TrackerTest, AFileDeletedInAWatchedDirectoryIsDeletedOnTheServer)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeUploads();

        std::filesystem::remove(root_ / "local" / "project" / "src" / "main.cpp");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                // FSEvents may replay the creation of src after the removal renewed its change time, its upload goes
                // first.
                completeUploads();
                return !remote_->of("remove").empty();
            }
        ));
        EXPECT_EQ(remote_->of("remove").front()->source, "/srv/project/src/main.cpp");
    }

    TEST_F(TrackerTest, ASlowMoveAsideSaveKeepsTheServerFileUnderItsName)
    {
        const auto notes = root_ / "local" / "notes.txt";
        const auto backup = root_ / "local" / "notes.txt~";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(notes), remote("/srv/notes.txt")).has_value());
        completeUploads();

        std::filesystem::rename(notes, backup);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return onlyEntry().name == "notes.txt~";
            }
        ));
        completeRemovesAndRenames();
        runFor(std::chrono::milliseconds{300});
        write(notes, "written anew");
        std::filesystem::remove(backup);

        // The upload waits in the server order until the rename back ended.
        EXPECT_TRUE(runUntil(
            [this]()
            {
                completeRemovesAndRenames();
                return uploadsTo("/srv/notes.txt") == 2u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(view_->questions.empty());
        EXPECT_EQ(onlyEntry().name, "notes.txt");
        EXPECT_EQ(onlyEntry().remotePath.value(), "/srv/notes.txt");
        const auto renames = remote_->of("rename");
        ASSERT_EQ(renames.size(), 2u);
        EXPECT_EQ(renames[0]->source, "/srv/notes.txt");
        EXPECT_EQ(renames[0]->destination, "/srv/notes.txt~");
        EXPECT_EQ(renames[1]->source, "/srv/notes.txt~");
        EXPECT_EQ(renames[1]->destination, "/srv/notes.txt");
    }

#ifdef __APPLE__
    TEST_F(TrackerTest, ChangesSpelledInAnotherNormalizationGoToTheFileTheServerHas)
    {
        const auto directory = root_ / "local" / "uni";
        const auto decomposed = directory / Paths::nativeFromUtf8("cafe\xCC\x81.txt");
        const auto composed = directory / Paths::nativeFromUtf8("caf\xC3\xA9.txt");
        std::filesystem::create_directories(directory);
        write(decomposed, "v1");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(directory), remote("/srv/uni")).has_value());
        // FSEvents also delivers the creation of the directory from just before the watch, it uploads it again.
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        remote_->server["/srv/uni/cafe\xCC\x81.txt"] = stateOf(decomposed);
        const auto uploadsBefore = remote_->of("upload").size();
        const auto decomposedUploadsBefore = uploadsTo("/srv/uni/cafe\xCC\x81.txt");

        // APFS finds the file under either spelling, efsw reports every name precomposed.
        std::ofstream{composed, std::ios::binary | std::ios::app} << " edited";
        ASSERT_TRUE(runUntil(
            [this, uploadsBefore]()
            {
                return remote_->of("upload").size() > uploadsBefore;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(uploadsTo("/srv/uni/cafe\xCC\x81.txt"), decomposedUploadsBefore + 1u);
        EXPECT_EQ(uploadsTo("/srv/uni/caf\xC3\xA9.txt"), 0u);
        completeUploads();
        std::filesystem::rename(decomposed, composed);

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty();
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/uni/cafe\xCC\x81.txt");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/uni/caf\xC3\xA9.txt");
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(uploadsTo("/srv/uni/caf\xC3\xA9.txt"), 0u);
    }

    TEST_F(TrackerTest, ChangesSpelledInAnotherNormalizationFindTheServerFileInEveryScript)
    {
        const auto directory = root_ / "local" / "kana";
        // A kana with the voiced sound mark, decomposed like Finder writes it and precomposed like efsw reports it.
        const auto decomposed = directory / Paths::nativeFromUtf8("\xE3\x81\x8B\xE3\x82\x99.txt");
        const auto composed = directory / Paths::nativeFromUtf8("\xE3\x81\x8C.txt");
        std::filesystem::create_directories(directory);
        write(decomposed, "v1");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(directory), remote("/srv/kana")).has_value());
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        remote_->server["/srv/kana/\xE3\x81\x8B\xE3\x82\x99.txt"] = stateOf(decomposed);
        const auto uploadsBefore = remote_->of("upload").size();
        const auto decomposedUploadsBefore = uploadsTo("/srv/kana/\xE3\x81\x8B\xE3\x82\x99.txt");

        std::ofstream{composed, std::ios::binary | std::ios::app} << " edited";
        ASSERT_TRUE(runUntil(
            [this, uploadsBefore]()
            {
                return remote_->of("upload").size() > uploadsBefore;
            }
        ));
        runFor(std::chrono::milliseconds{500});

        EXPECT_EQ(uploadsTo("/srv/kana/\xE3\x81\x8B\xE3\x82\x99.txt"), decomposedUploadsBefore + 1u);
        EXPECT_EQ(uploadsTo("/srv/kana/\xE3\x81\x8C.txt"), 0u);
    }

    TEST_F(TrackerTest, AFileInADecomposedDirectoryMirrorsItsEditsAndItsRename)
    {
        // Finder names in NFD, efsw reports every name precomposed.
        const auto directory = root_ / "local" / Paths::nativeFromUtf8("Cafe\xCC\x81");
        const auto notes = directory / "notes.txt";
        std::filesystem::create_directories(directory);
        write(notes, "v1");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(notes), remote("/srv/notes.txt")).has_value());
        // FSEvents also delivers the creation of the file from just before the watch.
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        const auto uploadsBefore = uploadsTo("/srv/notes.txt");

        write(notes, "v2, edited");
        ASSERT_TRUE(runUntil(
            [this, uploadsBefore]()
            {
                return uploadsTo("/srv/notes.txt") > uploadsBefore;
            }
        ));
        completeUploads();
        std::filesystem::rename(notes, directory / "renamed.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty();
            }
        ));
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/notes.txt");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/renamed.txt");
        EXPECT_EQ(onlyEntry().name, "renamed.txt");
        EXPECT_TRUE(remote_->of("remove").empty());
    }

    TEST_F(TrackerTest, AFileInADecomposedDirectoryRemovedLocallyIsRemovedOnTheServer)
    {
        const auto directory = root_ / "local" / Paths::nativeFromUtf8("Cafe\xCC\x81");
        const auto notes = directory / "notes.txt";
        std::filesystem::create_directories(directory);
        write(notes, "v1");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(notes), remote("/srv/notes.txt")).has_value());
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        const auto uploadsBefore = uploadsTo("/srv/notes.txt");

        std::filesystem::remove(notes);

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        EXPECT_EQ(remote_->of("remove").front()->source, "/srv/notes.txt");
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), uploadsBefore);
    }

    TEST_F(TrackerTest, ADecomposedDirectoryMirrorsTheChangesBelowIt)
    {
        const auto directory = root_ / "local" / Paths::nativeFromUtf8("Cafe\xCC\x81");
        std::filesystem::create_directories(directory);
        write(directory / "notes.txt", "v1");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(directory), remote("/srv/cafe")).has_value());
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        remote_->server["/srv/cafe/notes.txt"] = stateOf(directory / "notes.txt");
        const auto uploadsBefore = uploadsTo("/srv/cafe/notes.txt");

        write(directory / "notes.txt", "v2, edited");
        ASSERT_TRUE(runUntil(
            [this, uploadsBefore]()
            {
                return uploadsTo("/srv/cafe/notes.txt") > uploadsBefore;
            }
        ));
        completeUploads();
        const auto uploadsAfterTheEdit = uploadsTo("/srv/cafe/notes.txt");
        std::filesystem::remove(directory / "notes.txt");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        EXPECT_EQ(remote_->of("remove").front()->source, "/srv/cafe/notes.txt");
        EXPECT_EQ(uploadsTo("/srv/cafe/notes.txt"), uploadsAfterTheEdit);
    }
#endif

    TEST_F(TrackerTest, AnOptionTurnedOffIsNoLongerMirrored)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        tracker_->setOption(
            Ids::makeWatchId(onlyEntry().watchId.value()), SharedData::WatchOption::AutoReupload, false
        );
        EXPECT_FALSE(onlyEntry().options.autoReupload);
        write(opened->localPath.native(), "changed locally");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 0u);
    }

    TEST_F(TrackerTest, RemovingADownloadedCopyNeverDeletesTheServerFile)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto watchId = onlyEntry().watchId;

        tracker_->setOption(watchId, SharedData::WatchOption::DeleteRemoteOnLocalDelete, true);
        tracker_->setOption(watchId, SharedData::WatchOption::MoveRemoteOnLocalMove, true);
        std::filesystem::remove(opened->localPath.native());
        runFor(std::chrono::milliseconds{1000});

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(onlyEntry().downloadedCopy);
        EXPECT_FALSE(onlyEntry().options.deleteRemoteOnLocalDelete);
        EXPECT_FALSE(onlyEntry().options.moveRemoteOnLocalMove);
    }

    TEST_F(TrackerTest, ADownloadedCopyWrittenAnewAfterItsRemovalGoesToTheServer)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        std::filesystem::remove(opened->localPath.native());
        runFor(std::chrono::milliseconds{500});
        write(opened->localPath.native(), "written anew");

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        EXPECT_TRUE(remote_->of("remove").empty());
    }

    TEST_F(TrackerTest, StoppingADownloadedCopyEndsItsInstance)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto watchId = onlyEntry().watchId;

        tracker_->stop(watchId);

        EXPECT_TRUE(view_->entries.empty());
        EXPECT_EQ(view_->removed, std::vector<std::string>{watchId.value()});
        EXPECT_EQ(instances_->findInstance(opened->instanceId.value()), nullptr);
        EXPECT_FALSE(view_->orphanedCounts.empty());
        EXPECT_TRUE(open("/srv/notes.txt", /*watch=*/true).has_value());
    }

    TEST_F(TrackerTest, StoppingAnUploadedOriginalLeavesItAndFreesIt)
    {
        ASSERT_TRUE(
            tracker_->uploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv/notes.txt")).has_value()
        );

        tracker_->stop(onlyEntry().watchId);

        EXPECT_TRUE(view_->entries.empty());
        EXPECT_TRUE(std::filesystem::exists(root_ / "local" / "notes.txt"));
        EXPECT_TRUE(tracker_->checkUploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv")).has_value());
    }

    TEST_F(TrackerTest, DiscardingADownloadEndsItsWatchAndItsInstance)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        const auto discarded = tracker_->discard(opened->instanceId, opened->localPath);

        ASSERT_TRUE(discarded.has_value()) << discarded.error();
        EXPECT_EQ(*discarded, FileTracking::DiscardOutcome::InstanceDestroyed);
        EXPECT_TRUE(view_->entries.empty());
        EXPECT_FALSE(std::filesystem::exists(opened->localPath.native()));
    }

    TEST_F(TrackerTest, StoppingAllEndsEveryWatchWithoutTellingTheView)
    {
        ASSERT_TRUE(
            tracker_->uploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv/notes.txt")).has_value()
        );
        completeUploads();

        tracker_->stopAll();
        write(root_ / "local" / "notes.txt", "changed locally");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_TRUE(view_->removed.empty());
        EXPECT_TRUE(tracker_->checkUploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv")).has_value());
    }

    TEST_F(TrackerTest, AFileRecreatedBeforeItsDeletionIsHandledIsOnlyUploaded)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        // git rewrites a file by unlinking it and creating it anew.
        std::filesystem::remove(file);
        write(file, "int main() { return 1; }");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/main.cpp") == 1u || !view_->questions.empty();
            }
        ));

        EXPECT_TRUE(view_->questions.empty());
        EXPECT_TRUE(remote_->of("remove").empty());
    }

    TEST_F(TrackerTest, TheUploadAfterADeleteOfTheSamePathWaitsForItWithoutAQuestion)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::remove(file);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        write(file, "int main() { return 1; }");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_TRUE(view_->questions.empty());
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 0u);
        completeRemovesAndRenames();
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        EXPECT_TRUE(view_->questions.empty());
    }

    TEST_F(TrackerTest, TheDeleteOfADirectoryWaitsForTheDeletesOfItsContent)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::remove_all(root_ / "local" / "project" / "src");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        runFor(std::chrono::milliseconds{500});

        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/src/main.cpp"});
        completeRemovesAndRenames();
        EXPECT_EQ(
            remote_->sourcesOf("remove"), (std::vector<std::string>{"/srv/project/src/main.cpp", "/srv/project/src"})
        );
    }

    TEST_F(TrackerTest, TheWatchedDirectoryRemovedWithItsContentKeepsItsServerTreeOnlyOnMacOs)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));

        std::filesystem::remove_all(project);
        runFor(std::chrono::milliseconds{1500});
        completeEverything();

#ifdef __APPLE__
        // The accepted limitation, see Tracker: FSEvents reports only the removal of project, like a move to the trash.
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));
#else
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src"));
#endif
    }

    TEST_F(TrackerTest, ADirectoryMovedWithinTheItemLeavesNoServerCopyAtItsOldPlace)
    {
        const auto project = root_ / "local" / "project";
        std::filesystem::create_directories(project / "src" / "x" / "y");
        std::filesystem::create_directories(project / "docs");
        write(project / "src" / "x" / "main.c", "main");
        write(project / "src" / "x" / "y" / "deep.txt", "deep");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::rename(project / "src" / "x", project / "docs" / "x");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/docs/x") == 1u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        auto removed = remote_->sourcesOf("remove");
        std::ranges::sort(removed);
        EXPECT_EQ(
            removed,
            (std::vector<std::string>{
                "/srv/project/src/x",
                "/srv/project/src/x/main.c",
                "/srv/project/src/x/y",
                "/srv/project/src/x/y/deep.txt",
            })
        );
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/x/main.c"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/docs/x/y/deep.txt"));
        EXPECT_TRUE(view_->questions.empty());
    }

    TEST_F(TrackerTest, ADirectoryMovedOutAndInAgainLeavesNoServerCopyAtItsOldPlace)
    {
        const auto project = root_ / "local" / "project";
        std::filesystem::create_directories(project / "src" / "x");
        std::filesystem::create_directories(project / "docs");
        write(project / "src" / "x" / "main.c", "main");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        // Windows and macOS report a move between directories as a removal before the addition.
        std::filesystem::rename(project / "src" / "x", root_ / "local" / "x");
        runFor(std::chrono::milliseconds{1000});
        EXPECT_TRUE(remote_->of("remove").empty());
        std::filesystem::rename(root_ / "local" / "x", project / "docs" / "x");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/docs/x") == 1u;
            }
        ));
        completeEverything();

        auto removed = remote_->sourcesOf("remove");
        std::ranges::sort(removed);
        EXPECT_EQ(removed, (std::vector<std::string>{"/srv/project/src/x", "/srv/project/src/x/main.c"}));
    }

    TEST_F(TrackerTest, ADirectoryMovedOutOfTheItemKeepsItsServerCopy)
    {
        const auto project = root_ / "local" / "project";
        std::filesystem::create_directories(project / "src" / "x");
        write(project / "src" / "x" / "main.c", "main");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::rename(project / "src" / "x", root_ / "local" / "x");
        runFor(std::chrono::milliseconds{1000});
        completeEverything();

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/x/main.c"));
    }

    TEST_F(TrackerTest, AChangeOfAPathGoneMeanwhileRaisesNoQuestionAndDeletesItOnTheServer)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        remote_->server["/srv/project/src/main.cpp"].mtime += 100u;

        write(file, "changed");
        std::filesystem::remove(file);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty() || !view_->questions.empty();
            }
        ));
        runFor(std::chrono::milliseconds{500});

        EXPECT_TRUE(view_->questions.empty());
        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/src/main.cpp"});
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 0u);
    }

    TEST_F(TrackerTest, AFileTurnedIntoADirectoryWhileItsUploadIsOnItsWayReplacesTheServerFile)
    {
        const auto kind = root_ / "local" / "project" / "kind";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        write(kind, "file");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/kind") == 1u;
            }
        ));
        // git checkout turns the file into a directory while its upload runs, the server got the file.
        std::filesystem::remove(kind);
        std::filesystem::create_directory(kind);
        write(kind / "inside.txt", "inside");
        runFor(std::chrono::milliseconds{1000});
        completeWith(*remote_->of("upload").back(), FileTracking::ServerState{.mtime = 1, .size = 4});
        completeEverything();

        std::vector<std::string> atKind;
        for (auto const& operation : remote_->queued)
            if (operation.source == "/srv/project/kind" || operation.destination == "/srv/project/kind")
                atKind.push_back(operation.kind);
        EXPECT_EQ(atKind, (std::vector<std::string>{"upload", "remove", "upload"}));
        EXPECT_TRUE(view_->questions.empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/kind/inside.txt"));
    }

    TEST_F(TrackerTest, ADirectoryDeletedWhileItsUploadRunsButThereAgainAfterwardsIsUploadedAgain)
    {
        const auto directory = root_ / "local" / "project" / "dir";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::create_directory(directory);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/dir") == 1u;
            }
        ));
        std::filesystem::remove(directory);
        runFor(std::chrono::milliseconds{500});
        // git brings it back, its addition is reported after the upload ended. The upload sent it before, empty.
        std::filesystem::create_directory(directory);
        write(directory / "a.txt", "a");
        auto* first = remote_->of("upload").back();
        remote_->server["/srv/project/dir"] = FileTracking::ServerState{};
        first->ended = true;
        first->onUploadEnded({.reason = SharedData::OperationCompletionReason::Completed});
        completeEverything();

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(uploadsTo("/srv/project/dir"), 1u);
        EXPECT_EQ(uploadsTo("/srv/project/dir/a.txt"), 1u);
        EXPECT_TRUE(remote_->server.contains("/srv/project/dir/a.txt"));
        EXPECT_TRUE(view_->questions.empty());
    }

    TEST_F(TrackerTest, ALinkUploadedWithItsDirectoryIsDeletedOnTheServerToo)
    {
        const auto project = root_ / "local" / "project";
        std::error_code linkError;
        std::filesystem::create_directory_symlink("src", project / "link-to-src", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::remove(project / "link-to-src");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));

        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/link-to-src"});
    }

    TEST_F(TrackerTest, ALinkToADirectoryAddedLaterIsDeletedOnTheServerToo)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::error_code linkError;
        std::filesystem::create_directory_symlink("src", project / "link-to-src", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/link-to-src") == 1u;
            }
        ));
        completeEverything();
        std::filesystem::remove(project / "link-to-src");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));

        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/link-to-src"});
    }

    TEST_F(TrackerTest, ALinkRetargetedOverItselfRaisesNoQuestion)
    {
        const auto project = root_ / "local" / "project";
        std::error_code linkError;
        std::filesystem::create_symlink("src/main.cpp", project / "in", linkError);
        if (linkError)
            GTEST_SKIP() << "Cannot create a symbolic link: " << linkError.message();
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        // ln -sfn creates a link under a temporary name and renames it over the old one.
        write(project / "src" / "other.cpp", "other");
        std::filesystem::create_symlink("src/other.cpp", project / "in.retarget", linkError);
        std::filesystem::rename(project / "in.retarget", project / "in");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/in") == 1u || !view_->questions.empty();
            }
        ));

        EXPECT_TRUE(view_->questions.empty());
    }

    TEST_F(TrackerTest, AnOpenedCopyIsNotUploadedBackUnchanged)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());

        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 0u);
    }

    TEST_F(TrackerTest, UploadAndWatchUploadsWhatWasWrittenJustBeforeOnce)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeUploads();

        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(remote_->of("upload").size(), 1u);
    }

    TEST_F(TrackerTest, AFileCreatedWithItsDirectoryGoesOnceWhenItIsReportedAfterTheDirectoryUpload)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        const auto uploadsBefore = remote_->of("upload").size();

        // mkdir -p new/a/b && echo x > new/a/b/c.txt: the directory goes at once, the file once its change settled.
        std::filesystem::create_directories(project / "new" / "a" / "b");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new") == 1u;
            }
        ));
        write(project / "new" / "a" / "b" / "c.txt", "x");
        // The directory's upload read the file.
        completeUploads();
        runFor(std::chrono::milliseconds{1000});
        completeEverything();

        EXPECT_EQ(remote_->of("upload").size(), uploadsBefore + 1u);
        EXPECT_EQ(uploadsTo("/srv/project/new/a/b/c.txt"), 0u);
        EXPECT_TRUE(remote_->server.contains("/srv/project/new/a/b/c.txt"));
    }

    TEST_F(TrackerTest, ContentCreatedWithItsDirectoryGoesOnceWhenItIsReportedWhileTheDirectoryUploads)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        const auto uploadsBefore = remote_->of("upload").size();

        std::filesystem::create_directory(project / "new");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new") == 1u;
            }
        ));
        std::filesystem::create_directory(project / "new" / "sub");
        write(project / "new" / "one.txt", "one");
        write(project / "new" / "sub" / "two.txt", "two");
        // The changes come while the directory's upload runs, which read them all.
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_EQ(remote_->of("upload").size(), uploadsBefore + 1u);
        EXPECT_TRUE(remote_->server.contains("/srv/project/new/one.txt"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/new/sub/two.txt"));
    }

    TEST_F(TrackerTest, AFileChangedAfterItsDirectoryUploadSentItGoesAgain)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::create_directory(project / "new");
        write(project / "new" / "one.txt", "first");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new") == 1u;
            }
        ));
        completeUploads();
        write(project / "new" / "one.txt", "second, after the upload read the first");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new/one.txt") == 1u;
            }
        ));
        completeEverything();

        EXPECT_EQ(remote_->server.at("/srv/project/new/one.txt"), stateOf(project / "new" / "one.txt"));
        EXPECT_TRUE(view_->questions.empty());
    }

    TEST_F(TrackerTest, ASubdirectoryReportedWhileItsParentUploadsGoesWithIt)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        const auto uploadsBefore = remote_->of("upload").size();

        std::filesystem::create_directory(project / "new");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new") == 1u;
            }
        ));
        runFor(std::chrono::milliseconds{200});
        std::filesystem::create_directory(project / "new" / "sub");
        runFor(std::chrono::milliseconds{500});
        completeUploads();
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_EQ(remote_->of("upload").size(), uploadsBefore + 1u);
        EXPECT_TRUE(remote_->server.contains("/srv/project/new/sub"));
    }

    TEST_F(TrackerTest, TheProgressCountsTheEntriesOfADirectoryMovedIn)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(runUntilProgress({.pending = 0u, .done = 3u, .waitingForAnswer = 0u}));
        const auto staging = root_ / "staging";
        std::filesystem::create_directories(staging / "sub");
        write(staging / "one.txt", "one");
        write(staging / "sub" / "two.txt", "two");

        std::filesystem::rename(staging, project / "moved");

        ASSERT_TRUE(runUntilProgress({.pending = 4u, .done = 0u, .waitingForAnswer = 0u}));
        completeUploads();
        ASSERT_TRUE(runUntilProgress({.pending = 0u, .done = 4u, .waitingForAnswer = 0u}));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(lastProgress(), (SharedData::TrackerProgress{.pending = 0u, .done = 4u, .waitingForAnswer = 0u}));
    }

    TEST_F(TrackerTest, TheProgressCountsANewDirectoryAndTheLateReportsOfItsContentOnce)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::create_directory(project / "new");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/new") == 1u;
            }
        ));
        write(project / "new" / "one.txt", "one");
        write(project / "new" / "two.txt", "two");
        write(project / "new" / "three.txt", "three");
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return lastProgress().pending == 0u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_EQ(lastProgress(), (SharedData::TrackerProgress{.pending = 0u, .done = 4u, .waitingForAnswer = 0u}));
    }

    TEST_F(TrackerTest, TheProgressShowsAtOnceThatNothingIsOnItsWayAnyMore)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        write(opened->localPath.native(), "changed locally");
        ASSERT_TRUE(runUntilProgress({.pending = 1u, .done = 0u, .waitingForAnswer = 0u}));

        completeUploads();
        ioc_.restart();
        ioc_.run_for(std::chrono::milliseconds{50});

        EXPECT_EQ(lastProgress(), (SharedData::TrackerProgress{.pending = 0u, .done = 1u, .waitingForAnswer = 0u}));
        EXPECT_FALSE(onlyEntry().uploading);
    }
}
