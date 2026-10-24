#pragma once

#include <backend/file_tracking/directory_watch_service.hpp>
#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <backend/file_tracking/tracker.hpp>
#include <utility/localized_message.hpp>
#include <paths/native.hpp>
#include "error_log_capture.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <nui/utility/scope_exit.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#    include <unistd.h>
#endif

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
                OnUploadProgress onUploadProgress{};
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

            std::expected<void, std::string> upload(
                FileTracking::UploadRequest const& request,
                OnUploadEnded onEnded,
                OnUploadProgress onProgress
            ) override
            {
                queued.push_back({
                    .kind = "upload",
                    .source = request.source.toUtf8(),
                    .destination = request.destination.value(),
                    .upload = request,
                    .onEnded = {},
                    .onUploadEnded = std::move(onEnded),
                    .onUploadProgress = std::move(onProgress),
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

            void cancelUpload(Paths::RemotePath const& destination, FileTracking::PartialFile partial) override
            {
                cancels.emplace_back(destination.value(), partial);
            }

            std::expected<void, std::string>
            discardPartialUpload(Paths::RemotePath const& destination, OnEnded onEnded) override
            {
                queued.push_back({
                    .kind = "discard",
                    .source = destination.value(),
                    .destination = {},
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
            /** @brief The uploads the tracker stopped, by destination, in order. */
            std::vector<std::pair<std::string, FileTracking::PartialFile>> cancels{};
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

            void showEnded(SharedData::EndedWatches const& shown) override
            {
                ended.push_back(shown);
            }

            void ask(SharedData::ServerChangedQuestion const& question) override
            {
                ADD_FAILURE() << "the tracker lists conflicts, it asked about " << question.remotePath.value();
            }

            void showOrphanedCount(std::size_t count) override
            {
                orphanedCounts.push_back(count);
            }

            void showProgress(SharedData::TrackerProgress const& shown) override
            {
                progress.push_back(shown);
            }

            void showConflict(SharedData::WatchConflict const& conflict) override
            {
                conflicts.insert_or_assign(conflict.conflictId.value(), conflict);
                shownConflicts.push_back(conflict);
            }

            void removeConflict(Ids::ConflictId const& conflictId) override
            {
                conflicts.erase(conflictId.value());
                removedConflicts.push_back(conflictId.value());
            }

            void showFailure(SharedData::UploadFailure const& failure) override
            {
                failures.insert_or_assign(failure.failureId.value(), failure);
            }

            void removeFailure(Ids::FailureId const& failureId) override
            {
                failures.erase(failureId.value());
                removedFailures.push_back(failureId.value());
            }

            void showWatchProgress(SharedData::WatchProgress const& shown) override
            {
                watchProgress.push_back(shown);
            }

            void showOrphanedCopies(SharedData::OrphanedCopies const& copies) override
            {
                orphanedCopies.push_back(copies);
            }

            std::map<std::string, SharedData::TrackedEntry> entries{};
            std::vector<std::string> removed{};
            std::vector<SharedData::EndedWatches> ended{};
            std::vector<std::size_t> orphanedCounts{};
            std::vector<SharedData::TrackerProgress> progress{};
            /** @brief The conflicts shown and not removed, by id. */
            std::map<std::string, SharedData::WatchConflict> conflicts{};
            /** @brief Every conflict shown, in order. */
            std::vector<SharedData::WatchConflict> shownConflicts{};
            std::vector<std::string> removedConflicts{};
            /** @brief The failures shown and not removed, by id. */
            std::map<std::string, SharedData::UploadFailure> failures{};
            std::vector<std::string> removedFailures{};
            std::vector<SharedData::WatchProgress> watchProgress{};
            std::vector<SharedData::OrphanedCopies> orphanedCopies{};
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
                    {.reason = SharedData::OperationCompletionReason::Failed,
                        .changedOnServer = true,
                        .destinationState = serverStateAt(operation.destination)}
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

        /**
         * @brief Ends @p upload as @p ended tells, whatever the server has.
         */
        static void endAs(FakeRemote::Queued& upload, FileTracking::UploadEnd const& ended)
        {
            upload.ended = true;
            // The tracker queues what follows while it handles the end, which may move the queued operations.
            const auto onUploadEnded = std::move(upload.onUploadEnded);
            onUploadEnded(ended);
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
            // An upload skips what it cannot read (past MAX_PATH or being deleted on Windows).
            std::error_code statusError;
            const auto status = std::filesystem::symlink_status(source, statusError);
            if (std::filesystem::is_symlink(status))
                remote_->server[destination] = FileTracking::ServerState{};
            else if (std::filesystem::is_regular_file(status))
                remote_->server[destination] = stateOf(source);
            else if (std::filesystem::is_directory(status))
            {
                remote_->server[destination] = FileTracking::ServerState{};
                std::error_code listError;
                for (auto const& entry : std::filesystem::directory_iterator{source, listError})
                    serve(entry.path(), destination + "/" + entry.path().filename().generic_string());
            }
        }

        /**
         * @brief Ends every delete, rename and discard on its way as the server would.
         */
        void completeRemovesAndRenames()
        {
            for (std::size_t index = 0; index < remote_->queued.size(); ++index)
            {
                auto& operation = remote_->queued[index];
                if ((operation.kind != "remove" && operation.kind != "rename" && operation.kind != "discard") ||
                    operation.ended)
                    continue;
                // A discard removes the partial file of a stopped upload, which the fake server does not keep.
                if (const auto found = remote_->server.find(operation.source);
                    operation.kind != "discard" && found != remote_->server.end())
                {
                    if (operation.kind == "rename")
                        renameOnServer(operation.source, operation.destination);
                    else
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

        /**
         * @brief The server renames @p source to @p destination, a directory with its content.
         */
        void renameOnServer(std::string const& source, std::string const& destination)
        {
            std::vector<std::pair<std::string, FileTracking::ServerState>> moved;
            for (auto entry = remote_->server.begin(); entry != remote_->server.end();)
            {
                if (entry->first != source && !entry->first.starts_with(source + "/"))
                {
                    ++entry;
                    continue;
                }
                moved.emplace_back(destination + entry->first.substr(source.size()), entry->second);
                entry = remote_->server.erase(entry);
            }
            for (auto& [path, state] : moved)
                remote_->server.insert_or_assign(std::move(path), state);
        }

        /**
         * @brief Ends every upload the tracker stopped as the queue would, as cancelled.
         */
        void endStoppedUploads()
        {
            // Taken first: what follows a stop may queue the next upload to the same destination.
            std::vector<std::size_t> stopped;
            for (std::size_t index = 0; index < remote_->queued.size(); ++index)
            {
                auto const& operation = remote_->queued[index];
                const bool wasStopped = std::ranges::contains(
                    remote_->cancels | std::views::keys, operation.destination
                );
                if (operation.kind == "upload" && !operation.ended && wasStopped)
                    stopped.push_back(index);
            }
            for (const auto index : stopped)
            {
                remote_->queued[index].ended = true;
                // A copy: what follows may queue more and move the queued operations.
                const auto ended = remote_->queued[index].onUploadEnded;
                ended({.reason = SharedData::OperationCompletionReason::Canceled});
            }
        }

        /**
         * @brief Runs the handlers until the tracker stopped @p count uploads.
         */
        bool runUntilStopped(std::size_t count = 1u)
        {
            return runUntil(
                [this, count]()
                {
                    return remote_->cancels.size() >= count;
                }
            );
        }

        /**
         * @brief The upload to @p destination queued last, none when there is none.
         */
        FakeRemote::Queued* lastUploadTo(std::string const& destination)
        {
            FakeRemote::Queued* found = nullptr;
            for (auto* upload : remote_->of("upload"))
                if (upload->destination == destination)
                    found = upload;
            return found;
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

        SharedData::WatchConflict onlyConflict()
        {
            EXPECT_EQ(view_->conflicts.size(), 1u);
            return view_->conflicts.empty() ? SharedData::WatchConflict{} : view_->conflicts.begin()->second;
        }

        /**
         * @brief The conflict the view shows about @p localPath, none when it shows none.
         */
        std::optional<SharedData::WatchConflict> shownConflictAt(std::filesystem::path const& localPath) const
        {
            for (auto const& conflict : std::views::values(view_->conflicts))
                if (conflict.localPath.native() == localPath)
                    return conflict;
            return std::nullopt;
        }

        /**
         * @brief Runs the handlers until the view shows @p count conflicts.
         */
        bool runUntilConflicts(std::size_t count)
        {
            return runUntil(
                [this, count]()
                {
                    return view_->conflicts.size() == count;
                }
            );
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

    TEST_F(TrackerTest, AnOpenRefusedForANameTheLocalSystemCannotHoldIsToldToTheCallerOnly)
    {
        const ErrorLogCapture errors;
        const auto platform = std::exchange(Paths::defaultLocalPlatform(), Paths::LocalPlatform::Windows);
        auto restore = Nui::ScopeExit{[platform]() noexcept {
            Paths::defaultLocalPlatform() = platform;
        }};

        const auto opened = open("/srv/con", /*watch=*/true);

        ASSERT_FALSE(opened.has_value());
        EXPECT_TRUE(opened.error().has_value());
        EXPECT_TRUE(remote_->of("download").empty());
        EXPECT_EQ(errors.mentioning("/srv/con"), 0u);
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
        EXPECT_TRUE(view_->conflicts.empty());
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
        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(notes));
        EXPECT_EQ(remote_->stats, 0u);
    }

    TEST_F(TrackerTest, AServerCopyChangedMeanwhileIsListedAsAConflictOnceTheUploadFoundIt)
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
        EXPECT_TRUE(view_->conflicts.empty());
        completeUploads();
        ASSERT_EQ(view_->conflicts.size(), 1u);
        const auto conflict = onlyConflict();
        EXPECT_EQ(conflict.watchId, onlyEntry().watchId);
        EXPECT_EQ(conflict.kind, SharedData::ConflictKind::ChangedOnServer);
        EXPECT_EQ(conflict.localPath, opened->localPath);
        EXPECT_EQ(conflict.remotePath.value(), "/srv/notes.txt");
        EXPECT_EQ(conflict.serverModifiedAt, std::optional{changedOnServer.mtime});
        EXPECT_EQ(conflict.localModifiedAt, std::optional{stateOf(opened->localPath.native()).mtime});
        EXPECT_GT(conflict.raisedAt, 0u);
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), changedOnServer);
        EXPECT_TRUE(onlyEntry().uploading);

        tracker_->answerConflict(conflict.conflictId, SharedData::ConflictResolution::KeepServers);
        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(view_->removedConflicts, std::vector<std::string>{conflict.conflictId.value()});
        runFor(std::chrono::milliseconds{200});
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_FALSE(onlyEntry().uploading);
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), changedOnServer);

        // The server's version was kept, the next save raises the conflict again.
        write(opened->localPath.native(), "changed again");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 2u;
            }
        ));
        completeUploads();
        ASSERT_EQ(view_->conflicts.size(), 1u);
        const auto again = onlyConflict();
        EXPECT_NE(again.conflictId, conflict.conflictId);
        tracker_->answerConflict(again.conflictId, SharedData::ConflictResolution::ReplaceOnServer);
        EXPECT_TRUE(view_->conflicts.empty());
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 3u);
        EXPECT_FALSE(remote_->of("upload").back()->upload.expectedDestination.has_value());
        completeUploads();
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(opened->localPath.native()));
        EXPECT_FALSE(onlyEntry().uploading);
        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(remote_->stats, 0u);
    }

    TEST_F(TrackerTest, AFileTheServerLostIsListedAsDeletedOnTheServerBeforeItIsUploadedAgain)
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
        ASSERT_EQ(view_->conflicts.size(), 1u);
        const auto conflict = onlyConflict();
        EXPECT_EQ(conflict.kind, SharedData::ConflictKind::DeletedOnServer);
        EXPECT_FALSE(conflict.serverModifiedAt.has_value());
        EXPECT_TRUE(conflict.localModifiedAt.has_value());
        EXPECT_FALSE(remote_->server.contains("/srv/notes.txt"));
        tracker_->answerConflict(conflict.conflictId, SharedData::ConflictResolution::ReplaceOnServer);
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 2u);
        completeUploads();
        EXPECT_EQ(remote_->server.at("/srv/notes.txt"), stateOf(opened->localPath.native()));
    }

    TEST_F(TrackerTest, AFileThatAppearedOnTheServerBeforeItsFirstUploadIsListedAsCreatedOnTheServer)
    {
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        // FSEvents also delivers the creation of src from just before the watch. Coming after new.cpp, it would
        // upload what the server lacks below src without asking.
        runFor(std::chrono::milliseconds{1000});
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
        ASSERT_EQ(view_->conflicts.size(), 1u);
        const auto conflict = onlyConflict();
        EXPECT_EQ(conflict.kind, SharedData::ConflictKind::CreatedOnServer);
        EXPECT_EQ(conflict.remotePath.value(), "/srv/project/src/new.cpp");
        EXPECT_EQ(conflict.localPath.native(), root_ / "local" / "project" / "src" / "new.cpp");
        EXPECT_EQ(conflict.serverModifiedAt, std::optional{std::uint64_t{7u}});
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
        tracker_->answerConflict(onlyConflict().conflictId, SharedData::ConflictResolution::KeepServers);
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

    TEST_F(TrackerTest, AnAnswerToAnUnknownConflictChangesNothing)
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
        ASSERT_EQ(view_->conflicts.size(), 1u);
        tracker_->answerConflict(Ids::generateConflictId(), SharedData::ConflictResolution::ReplaceOnServer);
        tracker_->answerAllConflicts(Ids::generateWatchId(), SharedData::ConflictResolution::ReplaceOnServer);
        tracker_->answer(Ids::generateQuestionId(), SharedData::ServerChangedAnswer::Replace);

        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_TRUE(onlyEntry().uploading);
        EXPECT_EQ(view_->conflicts.size(), 1u);
        EXPECT_TRUE(view_->removedConflicts.empty());
    }

    /**
     * @brief Uploads and watches the project with a second file beside main.cpp, then changes both on the server
     *        and locally: each upload finds the server's copy changed and lists a conflict, main.cpp's first.
     */
    class TrackerConflictsTest : public TrackerTest
    {
      protected:
        std::filesystem::path main_;
        std::filesystem::path util_;

        void raiseTwoConflicts()
        {
            const auto source = root_ / "local" / "project" / "src";
            main_ = source / "main.cpp";
            util_ = source / "util.cpp";
            write(util_, "util");
            ASSERT_TRUE(
                tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value()
            );
            completeEverything();
            remote_->server["/srv/project/src/main.cpp"].mtime += 100u;
            remote_->server["/srv/project/src/util.cpp"].mtime += 100u;

            write(main_, "changed main");
            ASSERT_TRUE(runUntil(
                [this]()
                {
                    return uploadsTo("/srv/project/src/main.cpp") == 1u;
                }
            ));
            write(util_, "changed util");
            ASSERT_TRUE(runUntil(
                [this]()
                {
                    return uploadsTo("/srv/project/src/util.cpp") == 1u;
                }
            ));
            completeUploads();
            ASSERT_EQ(view_->conflicts.size(), 2u);
            ASSERT_TRUE(shownConflictAt(main_).has_value());
            ASSERT_TRUE(shownConflictAt(util_).has_value());
        }

        Ids::ConflictId conflictIdAt(std::filesystem::path const& localPath) const
        {
            return shownConflictAt(localPath).value_or(SharedData::WatchConflict{}).conflictId;
        }
    };

    TEST_F(TrackerConflictsTest, TwoConflictsOfADirectoryWatchAreListedAtOnceAndReplaceAllUploadsBothInOrder)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto mainConflict = conflictIdAt(main_);
        const auto utilConflict = conflictIdAt(util_);
        EXPECT_NE(mainConflict, utilConflict);
        EXPECT_EQ(view_->shownConflicts.front().conflictId, mainConflict);
        EXPECT_TRUE(onlyEntry().uploading);
        const auto uploadsBefore = remote_->of("upload").size();

        tracker_->answerAllConflicts(onlyEntry().watchId, SharedData::ConflictResolution::ReplaceOnServer);

        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(view_->removedConflicts, (std::vector<std::string>{mainConflict.value(), utilConflict.value()}));
        const auto uploads = remote_->sourcesOf("upload");
        ASSERT_EQ(uploads.size(), uploadsBefore + 2u);
        EXPECT_EQ(uploads[uploadsBefore], main_.string());
        EXPECT_EQ(uploads[uploadsBefore + 1u], util_.string());
        EXPECT_FALSE(lastUploadTo("/srv/project/src/main.cpp")->upload.expectedDestination.has_value());
        EXPECT_FALSE(lastUploadTo("/srv/project/src/util.cpp")->upload.expectedDestination.has_value());
        completeUploads();
        EXPECT_EQ(remote_->server.at("/srv/project/src/main.cpp"), stateOf(main_));
        EXPECT_EQ(remote_->server.at("/srv/project/src/util.cpp"), stateOf(util_));
        EXPECT_FALSE(onlyEntry().uploading);
    }

    TEST_F(TrackerConflictsTest, KeepAllKeepsTheServersVersionsAndTheNextSaveAsksAgain)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto keptMain = remote_->server.at("/srv/project/src/main.cpp");
        const auto keptUtil = remote_->server.at("/srv/project/src/util.cpp");

        tracker_->answerAllConflicts(onlyEntry().watchId, SharedData::ConflictResolution::KeepServers);

        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(view_->removedConflicts.size(), 2u);
        runFor(std::chrono::milliseconds{200});
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        EXPECT_EQ(uploadsTo("/srv/project/src/util.cpp"), 1u);
        EXPECT_EQ(remote_->server.at("/srv/project/src/main.cpp"), keptMain);
        EXPECT_EQ(remote_->server.at("/srv/project/src/util.cpp"), keptUtil);
        EXPECT_FALSE(onlyEntry().uploading);

        write(main_, "changed main again");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/main.cpp") == 2u;
            }
        ));
        completeUploads();
        ASSERT_EQ(view_->conflicts.size(), 1u);
        EXPECT_EQ(onlyConflict().localPath.native(), main_);
    }

    TEST_F(TrackerConflictsTest, ASingleReplaceAndASingleKeepAnswerOnlyTheirOwnConflict)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto keptUtil = remote_->server.at("/srv/project/src/util.cpp");

        tracker_->answerConflict(conflictIdAt(main_), SharedData::ConflictResolution::ReplaceOnServer);
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 2u);
        EXPECT_EQ(uploadsTo("/srv/project/src/util.cpp"), 1u);
        ASSERT_EQ(view_->conflicts.size(), 1u);
        EXPECT_EQ(onlyConflict().localPath.native(), util_);

        tracker_->answerConflict(conflictIdAt(util_), SharedData::ConflictResolution::KeepServers);
        EXPECT_TRUE(view_->conflicts.empty());
        completeUploads();
        runFor(std::chrono::milliseconds{200});
        EXPECT_EQ(uploadsTo("/srv/project/src/util.cpp"), 1u);
        EXPECT_EQ(remote_->server.at("/srv/project/src/main.cpp"), stateOf(main_));
        EXPECT_EQ(remote_->server.at("/srv/project/src/util.cpp"), keptUtil);
        EXPECT_FALSE(onlyEntry().uploading);
    }

    TEST_F(TrackerConflictsTest, StoppingAWatchDropsItsConflictsAndTellsTheView)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto mainConflict = conflictIdAt(main_);
        const auto utilConflict = conflictIdAt(util_);

        tracker_->stop(onlyEntry().watchId);

        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(view_->removedConflicts, (std::vector<std::string>{mainConflict.value(), utilConflict.value()}));
        tracker_->answerConflict(mainConflict, SharedData::ConflictResolution::ReplaceOnServer);
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);

        // The uploads that waited left the server order: a new watch of the same path goes at once.
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        EXPECT_EQ(uploadsTo("/srv/project"), 2u);
    }

    TEST_F(TrackerTest, StoppingADownloadedCopyWithAConflictFreesItsServerPath)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        remote_->server["/srv/notes.txt"].mtime += 100u;
        write(opened->localPath.native(), "changed locally");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        completeUploads();
        const auto conflictId = onlyConflict().conflictId;

        tracker_->stop(onlyEntry().watchId);
        EXPECT_EQ(view_->removedConflicts, std::vector<std::string>{conflictId.value()});

        const auto reopened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(reopened.has_value());
        write(reopened->localPath.native(), "changed in the new copy");
        EXPECT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 2u;
            }
        ));
    }

    TEST_F(TrackerConflictsTest, StoppingAllDropsTheConflictsWithoutTellingTheView)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());

        tracker_->stopAll();

        EXPECT_EQ(view_->conflicts.size(), 2u);
        EXPECT_TRUE(view_->removedConflicts.empty());
        tracker_->answerAllConflicts(
            view_->conflicts.begin()->second.watchId, SharedData::ConflictResolution::ReplaceOnServer
        );
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        EXPECT_EQ(uploadsTo("/srv/project"), 2u);
    }

    TEST_F(TrackerConflictsTest, ALostConnectionDropsTheConflictsAndTellsTheView)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto mainConflict = conflictIdAt(main_);
        const auto utilConflict = conflictIdAt(util_);

        tracker_->connectionLost();

        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(view_->removedConflicts, (std::vector<std::string>{mainConflict.value(), utilConflict.value()}));
        EXPECT_EQ(lastProgress().waitingForAnswer, 0u);
    }

    TEST_F(TrackerConflictsTest, AConflictWhosePathWasDeletedLocallyBeforeTheAnswerUploadsNothing)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        std::filesystem::remove(main_);
        // The deletion stops the upload, which already ended, and waits for its answer.
        ASSERT_TRUE(runUntilStopped());
        EXPECT_TRUE(remote_->of("remove").empty());

        tracker_->answerConflict(conflictIdAt(main_), SharedData::ConflictResolution::ReplaceOnServer);

        EXPECT_FALSE(shownConflictAt(main_).has_value());
        EXPECT_EQ(view_->conflicts.size(), 1u);
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        // Like any deletion, it goes to the server.
        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/src/main.cpp"});
    }

    TEST_F(TrackerConflictsTest, AConflictWhoseFileMovedAwayBeforeTheAnswerGoesToItsNewName)
    {
        ASSERT_NO_FATAL_FAILURE(raiseTwoConflicts());
        const auto renamed = main_.parent_path() / "renamed.cpp";
        std::filesystem::rename(main_, renamed);
        ASSERT_TRUE(runUntilStopped());

        tracker_->answerConflict(conflictIdAt(main_), SharedData::ConflictResolution::ReplaceOnServer);

        EXPECT_FALSE(shownConflictAt(main_).has_value());
        EXPECT_EQ(view_->conflicts.size(), 1u);
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        // The server's copy follows the rename, then the file goes over it.
        ASSERT_EQ(remote_->of("rename").size(), 1u);
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/project/src/main.cpp");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/project/src/renamed.cpp");
        completeRemovesAndRenames();
        ASSERT_EQ(uploadsTo("/srv/project/src/renamed.cpp"), 1u);
        EXPECT_FALSE(lastUploadTo("/srv/project/src/renamed.cpp")->upload.expectedDestination.has_value());
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
        EXPECT_TRUE(view_->conflicts.empty());
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
        runFor(FileTracking::copyReturnTime);
        EXPECT_TRUE(view_->ended.empty());
        EXPECT_EQ(view_->entries.size(), 1u);
    }

    TEST_F(TrackerTest, ADownloadedCopyMovedAsideAndWrittenAnewKeepsItsWatch)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto aside = opened->localPath.native().parent_path() / "notes.backup";

        std::filesystem::rename(opened->localPath.native(), aside);
        runFor(std::chrono::milliseconds{500});
        write(opened->localPath.native(), "saved anew");
        std::filesystem::remove(aside);

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
        runFor(FileTracking::copyReturnTime);
        EXPECT_TRUE(view_->ended.empty());
        EXPECT_EQ(view_->entries.size(), 1u);
        EXPECT_TRUE(remote_->of("rename").empty());
    }

    TEST_F(TrackerTest, ADownloadedCopyDeletedForGoodEndsItsWatchAndShowsWhy)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto watchId = onlyEntry().watchId;

        std::filesystem::remove(opened->localPath.native());
        runFor(std::chrono::milliseconds{1000});
        EXPECT_EQ(view_->entries.size(), 1u);

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->ended.empty();
            },
            FileTracking::copyReturnTime + std::chrono::seconds{2}
        ));
        EXPECT_EQ(view_->ended.front().reason, SharedData::WatchEndReason::CopyGone);
        ASSERT_EQ(view_->ended.front().entries.size(), 1u);
        EXPECT_EQ(view_->ended.front().entries.front().watchId, watchId);
        EXPECT_EQ(view_->ended.front().entries.front().name, "notes.txt");
        EXPECT_TRUE(view_->entries.empty());
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(instances_->findInstance(opened->instanceId.value()), nullptr);
        // Nothing was left in the instance, it went with its directory.
        EXPECT_FALSE(std::filesystem::exists(opened->localPath.native().parent_path()));
        EXPECT_EQ(instances_->countOrphaned(), 0u);
        EXPECT_TRUE(open("/srv/notes.txt", /*watch=*/true).has_value());
    }

    TEST_F(TrackerTest, ADownloadedCopyRenamedAwayEndsItsWatchAndStaysForTheCleanup)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto renamed = opened->localPath.native().parent_path() / "renamed.txt";

        std::filesystem::rename(opened->localPath.native(), renamed);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !view_->ended.empty();
            },
            FileTracking::copyReturnTime + std::chrono::seconds{2}
        ));
        write(renamed, "edited after the rename");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(view_->ended.front().reason, SharedData::WatchEndReason::CopyGone);
        EXPECT_TRUE(view_->entries.empty());
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 0u);
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_TRUE(std::filesystem::exists(renamed));
        EXPECT_EQ(instances_->countOrphaned(), 1u);
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

    TEST_F(TrackerTest, StoppingAllReleasesTheCopiesSoTheyCountAsOrphaned)
    {
        const auto watched = open("/srv/watched.txt", /*watch=*/true);
        const auto opened = open("/srv/opened.txt", /*watch=*/false);
        ASSERT_TRUE(watched.has_value());
        ASSERT_TRUE(opened.has_value());
        ASSERT_EQ(instances_->countOrphaned(), 0u);

        tracker_->stopAll();

        EXPECT_EQ(instances_->findInstance(watched->instanceId.value()), nullptr);
        EXPECT_EQ(instances_->findInstance(opened->instanceId.value()), nullptr);
        EXPECT_TRUE(std::filesystem::exists(watched->localPath.native()));
        EXPECT_TRUE(std::filesystem::exists(opened->localPath.native()));
        EXPECT_EQ(instances_->countOrphaned(), 2u);
        EXPECT_EQ(instances_->removeOrphaned(), 2);
    }

    TEST_F(TrackerTest, ALostConnectionEndsEveryWatchAndShowsWhy)
    {
        const auto copy = open("/srv/copy.txt", /*watch=*/true);
        ASSERT_TRUE(copy.has_value());
        ASSERT_TRUE(
            tracker_->uploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv/notes.txt")).has_value()
        );
        ASSERT_TRUE(runUntilProgress({.pending = 1}));

        tracker_->connectionLost();
        write(root_ / "local" / "notes.txt", "changed locally");
        write(copy->localPath.native(), "changed copy");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_TRUE(view_->entries.empty());
        EXPECT_EQ(view_->removed.size(), 2u);
        ASSERT_EQ(view_->ended.size(), 1u);
        EXPECT_EQ(view_->ended.front().reason, SharedData::WatchEndReason::ConnectionLost);
        const auto names = view_->ended.front().entries | std::views::transform(&SharedData::TrackedEntry::name) |
            std::ranges::to<std::set>();
        EXPECT_EQ(names, (std::set<std::string>{"copy.txt", "notes.txt"}));
        EXPECT_EQ(lastProgress(), SharedData::TrackerProgress{});
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 1u);
        EXPECT_EQ(uploadsTo("/srv/copy.txt"), 0u);
        EXPECT_EQ(instances_->findInstance(copy->instanceId.value()), nullptr);
        ASSERT_FALSE(view_->orphanedCounts.empty());
        EXPECT_EQ(view_->orphanedCounts.back(), 1u);
        EXPECT_TRUE(tracker_->checkUploadAndWatch(local(root_ / "local" / "notes.txt"), remote("/srv")).has_value());
    }

    TEST_F(TrackerTest, ALostConnectionWithoutWatchesShowsNoEnd)
    {
        tracker_->connectionLost();

        EXPECT_TRUE(view_->ended.empty());
        EXPECT_TRUE(view_->removed.empty());
    }

    TEST_F(TrackerTest, StoppingAllDropsADownloadOnItsWayWithItsInstance)
    {
        std::optional<Opened> opened;
        tracker_->open(
            remote("/srv/notes.txt"),
            0,
            /*watch=*/true,
            [&opened](Opened const& result)
            {
                opened = result;
            }
        );
        ASSERT_EQ(remote_->of("download").size(), 1u);

        tracker_->stopAll();
        end(*remote_->of("download").front(), SharedData::OperationCompletionReason::Completed);

        ASSERT_TRUE(opened.has_value());
        EXPECT_FALSE(opened->has_value());
        EXPECT_TRUE(view_->entries.empty());
        EXPECT_TRUE(instances_->listInstanceIds().empty());
        EXPECT_EQ(instances_->countOrphaned(), 0u);
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
                return uploadsTo("/srv/project/src/main.cpp") == 1u || !view_->conflicts.empty();
            }
        ));

        EXPECT_TRUE(view_->conflicts.empty());
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

        EXPECT_TRUE(view_->conflicts.empty());
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 0u);
        completeRemovesAndRenames();
        EXPECT_EQ(uploadsTo("/srv/project/src/main.cpp"), 1u);
        EXPECT_TRUE(view_->conflicts.empty());
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

    TEST_F(TrackerTest, TheWatchedDirectoryRemovedWithItsContentLeavesNoServerTree)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));

        std::filesystem::remove_all(project);

        // Windows also reports src modified, its delete waits until that change settled.
        EXPECT_TRUE(runUntil(
            [this]()
            {
                completeRemovesAndRenames();
                return !remote_->server.contains("/srv/project/src");
            }
        ));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
    }

    TEST_F(TrackerTest, TheWatchedDirectoryMovedAwayKeepsItsServerTree)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        std::filesystem::create_directories(root_ / "trash");

        std::filesystem::rename(project, root_ / "trash" / "project");
        runFor(std::chrono::milliseconds{1500});
        completeEverything();

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));
    }

    TEST_F(TrackerTest, TheWatchedDirectoryRemovedWithALargeTreeLeavesNoServerTree)
    {
        const auto project = root_ / "local" / "project";
        for (int directory = 0; directory < 8; ++directory)
        {
            const auto inner = project / ("d" + std::to_string(directory));
            std::filesystem::create_directories(inner);
            for (int file = 0; file < 8; ++file)
                write(inner / ("f" + std::to_string(file) + ".txt"), "content");
        }
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(remote_->server.contains("/srv/project/d7/f7.txt"));
        const auto serverTreeLeft = [this]()
        {
            return std::ranges::count_if(
                remote_->server,
                [](auto const& entry)
                {
                    return entry.first.starts_with("/srv/project/");
                }
            );
        };

        std::filesystem::remove_all(project);

        EXPECT_TRUE(runUntil(
            [&]()
            {
                completeRemovesAndRenames();
                return serverTreeLeft() == 0;
            },
            std::chrono::seconds{10}
        )) << serverTreeLeft() << " entries left";
    }

    TEST_F(TrackerTest, TheWatchedDirectoryRemovedWhileItsWatchIsBusyLeavesNoServerTree)
    {
        const auto project = root_ / "local" / "project";
        for (int directory = 0; directory < 4; ++directory)
        {
            const auto inner = project / ("d" + std::to_string(directory));
            std::filesystem::create_directories(inner);
            for (int file = 0; file < 8; ++file)
                write(inner / ("f" + std::to_string(file) + ".txt"), "content");
        }
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(remote_->server.contains("/srv/project/d3/f7.txt"));
        // Holds up efsw's thread at every event. Windows drops what changes meanwhile once the watched directory turns
        // delete-pending, efsw reports that as missed. Busy, a sleep on Windows lasts a timer tick and leaves efsw's
        // thread behind the removal of the item, whose watch ends before it hears of the lost changes.
        const auto busyListener = FileTracking::DirectoryWatchService::shared()->subscribe(
            project,
            /*recursive=*/true,
            [](FileTracking::DirectoryEvent const&)
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{1};
                while (std::chrono::steady_clock::now() < until)
                {
                }
            }
        );
        ASSERT_TRUE(busyListener.has_value());
        const auto serverTreeLeft = [this]()
        {
            return std::ranges::count_if(
                remote_->server,
                [](auto const& entry)
                {
                    return entry.first.starts_with("/srv/project/");
                }
            );
        };

        std::filesystem::remove_all(project);

        EXPECT_TRUE(runUntil(
            [&]()
            {
                completeRemovesAndRenames();
                return serverTreeLeft() == 0;
            },
            std::chrono::seconds{10}
        )) << serverTreeLeft() << " entries left";
    }

#ifdef __linux__
    TEST_F(TrackerTest, AChangeTheWatchMissedIsUploadedOnceItsDirectoryIsLookedAtAnew)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        // inotify reports a write to the watches of the directory it went through only.
        std::filesystem::create_directories(root_ / "elsewhere");
        std::filesystem::create_hard_link(project / "src" / "main.cpp", root_ / "elsewhere" / "main.cpp");
        write(root_ / "elsewhere" / "main.cpp", "int main() { return 2; }");
        runFor(std::chrono::milliseconds{1000});
        ASSERT_EQ(uploadsTo("/srv/project/src/main.cpp"), 0u);

        FileTracking::DirectoryWatchService::shared()->reportMissed(project / "src");

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/main.cpp") == 1u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(remote_->of("upload").size(), 2u);
        EXPECT_TRUE(remote_->of("remove").empty());
    }

    TEST_F(TrackerTest, AChangeOfADownloadedCopyTheWatchMissedIsUploadedOnceItsDirectoryIsLookedAtAnew)
    {
        const auto opened = open("/srv/notes.txt", /*watch=*/true);
        ASSERT_TRUE(opened.has_value());
        const auto copy = opened->localPath.native();
        runFor(std::chrono::milliseconds{500});

        FileTracking::DirectoryWatchService::shared()->reportMissed(copy.parent_path());
        runFor(std::chrono::milliseconds{500});
        EXPECT_EQ(uploadsTo("/srv/notes.txt"), 0u);

        std::filesystem::create_directories(root_ / "elsewhere");
        std::filesystem::create_hard_link(copy, root_ / "elsewhere" / "notes.txt");
        write(root_ / "elsewhere" / "notes.txt", "edited in the copy");
        runFor(std::chrono::milliseconds{1000});
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 0u);
        FileTracking::DirectoryWatchService::shared()->reportMissed(copy.parent_path());

        EXPECT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/notes.txt") == 1u;
            }
        ));
    }
#endif

    TEST_F(TrackerTest, ADeletionTheWatchMissedGoesToTheServerOnceItsDirectoryIsLookedAtAnew)
    {
        const auto project = root_ / "local" / "project";
        write(project / "src" / "util.cpp", "util");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        // Out of the item the deletion reaches no watch, the server keeps the copy of a directory that left.
        std::filesystem::rename(project / "src", root_ / "src");
        runFor(std::chrono::milliseconds{1000});
        std::filesystem::remove(root_ / "src" / "main.cpp");
        std::filesystem::rename(root_ / "src", project / "src");
        runFor(std::chrono::milliseconds{1000});
        completeEverything();
        ASSERT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));

        FileTracking::DirectoryWatchService::shared()->reportMissed(project);

        EXPECT_TRUE(runUntil(
            [this]()
            {
                completeRemovesAndRenames();
                return !remote_->server.contains("/srv/project/src/main.cpp");
            }
        ));
        runFor(std::chrono::milliseconds{500});
        completeEverything();
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/util.cpp"));
        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/src/main.cpp"});
    }

    TEST_F(TrackerTest, AWatchedDirectoryGoneWhenItsWatchMissedTheChangesIsDeletedWithItsTreeOnlyOnWindows)
    {
        const auto project = root_ / "local" / "project";
        write(project / "src" / "util.cpp", "util");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        // Like a watched directory deleted on Windows without POSIX semantics, which loses the last deletions.
        std::filesystem::rename(project / "src", root_ / "src");
        runFor(std::chrono::milliseconds{1000});
        std::filesystem::remove_all(root_ / "src");
        ASSERT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));

        FileTracking::DirectoryWatchService::shared()->reportMissed(project / "src");

#ifdef _WIN32
        EXPECT_TRUE(runUntil(
            [this]()
            {
                completeRemovesAndRenames();
                return !remote_->server.contains("/srv/project/src");
            }
        ));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/util.cpp"));
#else
        // It may have gone to the trash.
        runFor(std::chrono::milliseconds{1000});
        completeEverything();
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/util.cpp"));
#endif
        EXPECT_TRUE(remote_->server.contains("/srv/project"));
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
                return !remote_->of("rename").empty() || uploadsTo("/srv/project/docs/x") == 1u;
            }
        ));
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_FALSE(remote_->server.contains("/srv/project/src/x/main.c"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/x"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/docs/x/y/deep.txt"));
        EXPECT_TRUE(view_->conflicts.empty());
#ifndef __APPLE__
        // The watch reports the move between directories as one, the server renames the directory.
        ASSERT_EQ(remote_->of("rename").size(), 1u);
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/project/src/x");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/project/docs/x");
        EXPECT_EQ(uploadsTo("/srv/project/docs/x"), 0u);
        EXPECT_TRUE(remote_->of("remove").empty());
#endif
    }

    TEST_F(TrackerTest, AFileMovedToAnotherDirectoryOfTheItemIsRenamedOnTheServer)
    {
        const auto project = root_ / "local" / "project";
        std::filesystem::create_directories(project / "lib");
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::rename(project / "src" / "main.cpp", project / "lib" / "main.cpp");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty() || !remote_->of("remove").empty();
            }
        ));
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/lib/main.cpp"));
        EXPECT_TRUE(view_->conflicts.empty());
#ifndef __APPLE__
        ASSERT_EQ(remote_->of("rename").size(), 1u);
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/project/src/main.cpp");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/project/lib/main.cpp");
        EXPECT_EQ(uploadsTo("/srv/project/lib/main.cpp"), 0u);
        EXPECT_TRUE(remote_->of("remove").empty());
#endif
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
                return !remote_->of("remove").empty() || !view_->conflicts.empty();
            }
        ));
        runFor(std::chrono::milliseconds{500});

        EXPECT_TRUE(view_->conflicts.empty());
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
        EXPECT_TRUE(view_->conflicts.empty());
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
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, ADirectoryRemovedJustAfterItsUploadSentItLeavesNothingOnTheServer)
    {
        const auto project = root_ / "local" / "project";
        const auto directory = project / "moved-dir";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        // git checks out a branch with the directory, the next checkout removes it again while it uploads.
        std::filesystem::create_directory(directory);
        write(directory / "m0.txt", "m0");
        write(directory / "m1.txt", "m1");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/moved-dir") == 1u;
            }
        ));
        auto* upload = remote_->of("upload").back();
        const auto sent = sentFilesOf(directory);
        serve(directory, "/srv/project/moved-dir");
        // Gone before the upload ended, the removals are reported afterwards.
        std::filesystem::remove_all(directory);
        endAs(*upload, {.reason = SharedData::OperationCompletionReason::Completed, .sentFiles = sent});
        runFor(std::chrono::milliseconds{1000});
        completeEverything();

        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir/m0.txt"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir/m1.txt"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir"));
        const auto removed = remote_->sourcesOf("remove");
        ASSERT_FALSE(removed.empty());
        EXPECT_EQ(removed.back(), "/srv/project/moved-dir");
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, ADirectoryRemovedWhileItUploadsLeavesNothingOfWhatTheUploadSent)
    {
        const auto project = root_ / "local" / "project";
        const auto directory = project / "moved-dir";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::create_directory(directory);
        write(directory / "m0.txt", "m0");
        write(directory / "m1.txt", "m1");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/moved-dir") == 1u;
            }
        ));
        auto* upload = remote_->of("upload").back();
        auto sent = sentFilesOf(directory);
        // m0.txt went while it was read, the upload tells it without a state.
        for (auto& file : sent)
            if (file.path.filename()->view() == "m0.txt")
                file.state.reset();
        serve(directory, "/srv/project/moved-dir");
        std::filesystem::remove_all(directory);
        runFor(std::chrono::milliseconds{1000});
        endAs(*upload, {.reason = SharedData::OperationCompletionReason::Completed, .sentFiles = sent});
        completeEverything();

        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir/m0.txt"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir/m1.txt"));
        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir"));
        const auto removed = remote_->sourcesOf("remove");
        ASSERT_FALSE(removed.empty());
        EXPECT_EQ(removed.back(), "/srv/project/moved-dir");
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, AnEmptyDirectoryAnUploadLeftAfterItsFilesWentIsDeletedOnTheServer)
    {
        const auto project = root_ / "local" / "project";
        const auto directory = project / "moved-dir";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::create_directory(directory);
        write(directory / "m0.txt", "m0");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/moved-dir") == 1u;
            }
        ));
        auto* upload = remote_->of("upload").back();
        // The upload created the directory, its files were gone by the time it got to them.
        remote_->server["/srv/project/moved-dir"] = FileTracking::ServerState{};
        std::filesystem::remove_all(directory);
        endAs(*upload, {.reason = SharedData::OperationCompletionReason::Completed});
        runFor(std::chrono::milliseconds{1000});
        completeEverything();

        EXPECT_FALSE(remote_->server.contains("/srv/project/moved-dir"));
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, AFileDeletedWhileItsDirectoryUploadsIsDeletedOnTheServerIfTheUploadSentIt)
    {
        const auto project = root_ / "local" / "project";
        const auto staging = root_ / "staging";
        const auto directory = project / "moved-in";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        std::filesystem::create_directory(staging);
        write(staging / "keep.txt", "keep");
        write(staging / "gone.txt", "gone");

        // Moved in, its files are reported with it only.
        std::filesystem::rename(staging, directory);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/moved-in") == 1u;
            }
        ));
        auto* upload = remote_->of("upload").back();
        const auto sent = sentFilesOf(directory);
        serve(directory, "/srv/project/moved-in");
        // An editor's temporary file, its removal is reported while the upload still runs.
        std::filesystem::remove(directory / "gone.txt");
        runFor(std::chrono::milliseconds{1000});
        endAs(*upload, {.reason = SharedData::OperationCompletionReason::Completed, .sentFiles = sent});
        completeEverything();

        EXPECT_EQ(remote_->sourcesOf("remove"), std::vector<std::string>{"/srv/project/moved-in/gone.txt"});
        EXPECT_TRUE(remote_->server.contains("/srv/project/moved-in/keep.txt"));
        EXPECT_TRUE(view_->conflicts.empty());
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
                return uploadsTo("/srv/project/in") == 1u || !view_->conflicts.empty();
            }
        ));

        EXPECT_TRUE(view_->conflicts.empty());
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
        EXPECT_TRUE(view_->conflicts.empty());
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

    TEST_F(TrackerTest, TheProgressCountsWhatADirectoryUploadLeftNotChangesThatCameToNothing)
    {
        const auto project = root_ / "local" / "project";
        const auto staging = root_ / "staging";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();
        ASSERT_TRUE(runUntilProgress({.pending = 0u, .done = 3u, .waitingForAnswer = 0u}));
        std::filesystem::create_directory(staging);
        write(staging / "one.txt", "one");

        std::filesystem::rename(staging, project / "moved-in");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/moved-in") == 1u;
            }
        ));
        // An editor's temporary files come and go while the directory uploads.
        for (auto const* name : {"a.tmp", "b.tmp", "c.tmp"})
        {
            write(project / "moved-in" / name, "temporary");
            std::filesystem::remove(project / "moved-in" / name);
        }
        runFor(std::chrono::milliseconds{1000});
        completeUploads();
        runFor(std::chrono::milliseconds{500});
        completeEverything();

        EXPECT_TRUE(runUntilProgress({.pending = 0u, .done = 2u, .waitingForAnswer = 0u}));
        EXPECT_TRUE(remote_->of("remove").empty());
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

#ifdef _WIN32
    TEST_F(TrackerTest, EntriesPastMaxPathAreNeitherUploadedNorDeletedOnTheServer)
    {
        const auto project = root_ / "local" / "project";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(project), remote("/srv/project")).has_value());
        completeEverything();

        // Without long path support std::filesystem does not find these, although they exist.
        const std::wstring segment(48, L's');
        auto deep = project;
        for (int level = 0; level < 6; ++level)
            deep /= segment;
        const auto extended = [](std::filesystem::path const& path)
        {
            return std::filesystem::path{LR"(\\?\)" + path.lexically_normal().make_preferred().native()};
        };
        auto removeTree = Nui::ScopeExit{[&]() noexcept {
            std::error_code removeError;
            std::filesystem::remove_all(extended(project / segment), removeError);
        }};
        std::filesystem::create_directories(extended(deep));
        std::ofstream{extended(deep / "long-file.txt")} << "long";
        ASSERT_GE((deep / "long-file.txt").native().size(), std::size_t{MAX_PATH});
        for (int round = 0; round < 10; ++round)
        {
            completeUploads();
            runFor(std::chrono::milliseconds{200});
        }

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(
            std::ranges::count_if(
                remote_->server,
                [](auto const& entry)
                {
                    return entry.first.ends_with("/long-file.txt");
                }
            ),
            0
        );
    }
#else
    TEST_F(TrackerTest, ADeletionThatCannotBeCheckedKeepsTheServerCopy)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can look into any directory";
        const auto source = root_ / "local" / "project" / "src";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        // Deleted, but its directory no longer lets anyone look: for the tracker it may as well still be there.
        std::filesystem::remove(source / "main.cpp");
        std::filesystem::permissions(source, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&source]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(source, std::filesystem::perms::owner_all, errorCode);
        }};
        runFor(std::chrono::milliseconds{1000});

        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->server.contains("/srv/project/src/main.cpp"));
    }

    TEST_F(TrackerTest, AChangeThatCannotBeCheckedIsNotUploaded)
    {
        if (::geteuid() == 0)
            GTEST_SKIP() << "root can look into any directory";
        const auto source = root_ / "local" / "project" / "src";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        write(source / "new.cpp", "new");
        std::filesystem::permissions(source, std::filesystem::perms::none);
        auto restore = Nui::ScopeExit{[&source]() noexcept {
            std::error_code errorCode{};
            std::filesystem::permissions(source, std::filesystem::perms::owner_all, errorCode);
        }};
        runFor(std::chrono::milliseconds{1000});

        EXPECT_EQ(uploadsTo("/srv/project/src/new.cpp"), 0u);
        EXPECT_TRUE(remote_->of("remove").empty());
    }
#endif

    TEST_F(TrackerTest, ANewFileDeletedWhileItUploadsStopsTheUploadAndDeletesNothing)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 1u;
            }
        ));

        std::filesystem::remove(file);

        ASSERT_TRUE(runUntilStopped());
        EXPECT_EQ(remote_->cancels.front().first, "/srv/project/big.bin");
        EXPECT_EQ(remote_->cancels.front().second, FileTracking::PartialFile::Discarded);
        endStoppedUploads();
        completeEverything();
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->of("discard").empty());
        EXPECT_FALSE(remote_->server.contains("/srv/project/big.bin"));
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, AKnownFileDeletedWhileItsChangeUploadsStopsItThenDeletesTheServerCopy)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "int main() { return 1; }");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/main.cpp") == 1u;
            }
        ));

        std::filesystem::remove(file);

        ASSERT_TRUE(runUntilStopped());
        endStoppedUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        EXPECT_EQ(remote_->of("remove").front()->source, "/srv/project/src/main.cpp");
        EXPECT_TRUE(remote_->of("discard").empty());
    }

    TEST_F(TrackerTest, AnUploadThatFinishedBeforeItsStopStillDeletesTheServerCopy)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 1u;
            }
        ));
        std::filesystem::remove(file);
        ASSERT_TRUE(runUntilStopped());

        completeWith(*lastUploadTo("/srv/project/big.bin"), FileTracking::ServerState{.mtime = 1u, .size = 13u});

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("remove").empty();
            }
        ));
        EXPECT_EQ(remote_->of("remove").front()->source, "/srv/project/big.bin");
    }

    TEST_F(TrackerTest, AFileDeletedAndWrittenAgainWhileItUploadsGoesOnceMore)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 1u;
            }
        ));
        std::filesystem::remove(file);
        ASSERT_TRUE(runUntilStopped());

        write(file, "written again");
        runFor(std::chrono::milliseconds{500});
        endStoppedUploads();

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 2u;
            }
        ));
        completeEverything();
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(remote_->server.at("/srv/project/big.bin"), stateOf(file));
    }

    TEST_F(TrackerTest, ADeleteReportedAfterTheFileCameBackStopsNothing)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 1u;
            }
        ));

        // Both happen before the tracker hears of the deletion, as when Windows holds it back.
        std::filesystem::remove(file);
        write(file, "written again");
        runFor(std::chrono::milliseconds{1000});

        EXPECT_TRUE(remote_->cancels.empty());
        completeEverything();
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_EQ(remote_->server.at("/srv/project/big.bin"), stateOf(file));
    }

    TEST_F(TrackerTest, AFileMovedOverAKnownFileWhileItUploadsStopsIt)
    {
        const auto file = root_ / "local" / "project" / "src" / "new.cpp";
        const auto known = root_ / "local" / "project" / "src" / "main.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "int main() { return 2; }");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/new.cpp") == 1u;
            }
        ));

        std::filesystem::rename(file, known);

        ASSERT_TRUE(runUntilStopped());
        EXPECT_EQ(remote_->cancels.front().first, "/srv/project/src/new.cpp");
        EXPECT_EQ(remote_->cancels.front().second, FileTracking::PartialFile::Discarded);
        endStoppedUploads();
        completeEverything();
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_EQ(remote_->server.at("/srv/project/src/main.cpp"), stateOf(known));
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/new.cpp"));
    }

    TEST_F(TrackerTest, ADirectoryDeletedWhileItUploadsIsNotStopped)
    {
        const auto directory = root_ / "local" / "project" / "dir";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        std::filesystem::create_directory(directory);
        write(directory / "a.txt", "a");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/dir") == 1u;
            }
        ));

        std::filesystem::remove_all(directory);
        runFor(std::chrono::milliseconds{1000});

        for (auto const& destination : std::views::keys(remote_->cancels))
            ADD_FAILURE() << "stopped the upload to " << destination;
    }

    TEST_F(TrackerTest, TheProgressCountsAStoppedUploadAsDone)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        runFor(std::chrono::milliseconds{500});
        write(file, "large content");
        ASSERT_TRUE(runUntilProgress({.pending = 1u, .done = 0u, .waitingForAnswer = 0u}));

        std::filesystem::remove(file);
        ASSERT_TRUE(runUntilStopped());
        endStoppedUploads();

        EXPECT_TRUE(runUntilProgress({.pending = 0u, .done = 1u, .waitingForAnswer = 0u}));
    }

    TEST_F(TrackerTest, StoppingAWatchLeavesItsRunningUploadAlone)
    {
        const auto file = root_ / "local" / "project" / "big.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/big.bin") == 1u;
            }
        ));

        tracker_->stop(onlyEntry().watchId);
        runFor(std::chrono::milliseconds{200});

        EXPECT_TRUE(remote_->cancels.empty());
        EXPECT_FALSE(lastUploadTo("/srv/project/big.bin")->ended);
    }

    TEST_F(TrackerTest, AFileRenamedWhileItsFirstUploadRunsContinuesUnderTheNewName)
    {
        const auto file = root_ / "local" / "project" / "huge.bin";
        const auto renamed = root_ / "local" / "project" / "renamed.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/huge.bin") == 1u;
            }
        ));

        std::filesystem::rename(file, renamed);

        ASSERT_TRUE(runUntilStopped());
        EXPECT_EQ(remote_->cancels.front().first, "/srv/project/huge.bin");
        EXPECT_EQ(remote_->cancels.front().second, FileTracking::PartialFile::Kept);
        endStoppedUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/renamed.bin") == 1u;
            }
        ));
        EXPECT_EQ(
            lastUploadTo("/srv/project/renamed.bin")->upload.continuesPartialOf, remote("/srv/project/huge.bin")
        );
        completeEverything();
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_TRUE(remote_->of("remove").empty());
        EXPECT_TRUE(remote_->of("discard").empty());
        EXPECT_EQ(uploadsTo("/srv/project/huge.bin"), 1u);
        EXPECT_FALSE(remote_->server.contains("/srv/project/huge.bin"));
        EXPECT_EQ(remote_->server.at("/srv/project/renamed.bin"), stateOf(renamed));
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, AKnownFileRenamedWhileItsChangeUploadsRenamesTheServerCopyThenContinues)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        const auto renamed = root_ / "local" / "project" / "src" / "start.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "int main() { return 3; }");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/main.cpp") == 1u;
            }
        ));

        std::filesystem::rename(file, renamed);

        ASSERT_TRUE(runUntilStopped());
        endStoppedUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty();
            }
        ));
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/project/src/main.cpp");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/project/src/start.cpp");
        // The upload under the new name waits for the rename.
        EXPECT_EQ(uploadsTo("/srv/project/src/start.cpp"), 0u);
        completeRemovesAndRenames();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/start.cpp") == 1u;
            }
        ));
        EXPECT_EQ(
            lastUploadTo("/srv/project/src/start.cpp")->upload.continuesPartialOf, remote("/srv/project/src/main.cpp")
        );
        completeEverything();
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_EQ(remote_->server.at("/srv/project/src/start.cpp"), stateOf(renamed));
        EXPECT_TRUE(view_->conflicts.empty());
    }

    TEST_F(TrackerTest, AFileRenamedTwiceWhileItUploadsContinuesUnderTheLastName)
    {
        const auto file = root_ / "local" / "project" / "a.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u;
            }
        ));

        std::filesystem::rename(file, root_ / "local" / "project" / "b.bin");
        ASSERT_TRUE(runUntilStopped());
        std::filesystem::rename(root_ / "local" / "project" / "b.bin", root_ / "local" / "project" / "c.bin");
        ASSERT_TRUE(runUntilStopped(2u));
        endStoppedUploads();

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/c.bin") == 1u;
            }
        ));
        EXPECT_EQ(lastUploadTo("/srv/project/c.bin")->upload.continuesPartialOf, remote("/srv/project/a.bin"));
        completeEverything();
        EXPECT_EQ(uploadsTo("/srv/project/b.bin"), 0u);
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_TRUE(remote_->of("discard").empty());
    }

    TEST_F(TrackerTest, AFileRenamedThenDeletedWhileItUploadsLeavesNoPartialFile)
    {
        const auto file = root_ / "local" / "project" / "a.bin";
        const auto renamed = root_ / "local" / "project" / "b.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u;
            }
        ));

        std::filesystem::rename(file, renamed);
        ASSERT_TRUE(runUntilStopped());
        std::filesystem::remove(renamed);
        runFor(std::chrono::milliseconds{500});
        endStoppedUploads();

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("discard").empty();
            }
        ));
        EXPECT_EQ(remote_->of("discard").front()->source, "/srv/project/a.bin");
        EXPECT_EQ(uploadsTo("/srv/project/b.bin"), 0u);
        EXPECT_TRUE(remote_->of("remove").empty());
    }

    TEST_F(TrackerTest, ARenameAfterTheUploadFinishedRenamesTheServerCopy)
    {
        const auto file = root_ / "local" / "project" / "a.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u;
            }
        ));
        std::filesystem::rename(file, root_ / "local" / "project" / "b.bin");
        ASSERT_TRUE(runUntilStopped());

        completeWith(*lastUploadTo("/srv/project/a.bin"), FileTracking::ServerState{.mtime = 1u, .size = 13u});

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty();
            }
        ));
        EXPECT_EQ(remote_->of("rename").front()->source, "/srv/project/a.bin");
        EXPECT_EQ(remote_->of("rename").front()->destination, "/srv/project/b.bin");
        completeEverything();
        EXPECT_EQ(uploadsTo("/srv/project/b.bin"), 0u);
        EXPECT_TRUE(remote_->of("discard").empty());
    }

    TEST_F(TrackerTest, AWatchedFileRenamedWhileItUploadsContinuesUnderTheNewName)
    {
        const auto notes = root_ / "local" / "notes.txt";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(notes), remote("/srv/notes.txt")).has_value());
        ASSERT_EQ(uploadsTo("/srv/notes.txt"), 1u);

        std::filesystem::rename(notes, root_ / "local" / "memo.txt");

        ASSERT_TRUE(runUntilStopped());
        EXPECT_EQ(remote_->cancels.front().first, "/srv/notes.txt");
        EXPECT_EQ(remote_->cancels.front().second, FileTracking::PartialFile::Kept);
        endStoppedUploads();
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/memo.txt") == 1u;
            }
        ));
        EXPECT_EQ(lastUploadTo("/srv/memo.txt")->upload.continuesPartialOf, remote("/srv/notes.txt"));
        completeEverything();
        EXPECT_EQ(onlyEntry().remotePath.value(), "/srv/memo.txt");
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_FALSE(remote_->server.contains("/srv/notes.txt"));
    }

    TEST_F(TrackerTest, AFileMovedOverAFileWhoseFirstUploadRunsReplacesIt)
    {
        const auto first = root_ / "local" / "project" / "a.bin";
        const auto second = root_ / "local" / "project" / "b.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(first, "first content");
        write(second, "second");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u && uploadsTo("/srv/project/b.bin") == 1u;
            }
        ));

        std::filesystem::rename(first, second);

        ASSERT_TRUE(runUntilStopped());
        EXPECT_EQ(remote_->cancels.front().first, "/srv/project/a.bin");
        EXPECT_EQ(remote_->cancels.front().second, FileTracking::PartialFile::Discarded);
        endStoppedUploads();
        completeEverything();
        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_FALSE(remote_->server.contains("/srv/project/a.bin"));
        EXPECT_EQ(remote_->server.at("/srv/project/b.bin"), stateOf(second));
    }

    TEST_F(TrackerTest, AFileMovedIntoANewDirectoryWhileItUploadsLeavesNoOldServerCopy)
    {
        const auto known = root_ / "local" / "project" / "src" / "main.cpp";
        const auto directory = root_ / "local" / "project" / "x";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        std::filesystem::create_directory(directory);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/x") == 1u;
            }
        ));

        std::filesystem::rename(known, directory / "main.cpp");
        runFor(std::chrono::milliseconds{1000});
        completeEverything();

        EXPECT_TRUE(remote_->of("rename").empty());
        EXPECT_FALSE(remote_->server.contains("/srv/project/src/main.cpp"));
        EXPECT_TRUE(remote_->server.contains("/srv/project/x/main.cpp"));
    }

    TEST_F(TrackerTest, WithoutMirroredMovesARenamedFirstUploadStillUploadsTheNewName)
    {
        const auto file = root_ / "local" / "project" / "a.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        tracker_->setOption(onlyEntry().watchId, SharedData::WatchOption::MoveRemoteOnLocalMove, false);
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u;
            }
        ));

        std::filesystem::rename(file, root_ / "local" / "project" / "b.bin");

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/b.bin") == 1u;
            }
        ));
        // A watch that reads the rename as a deletion and an addition stops the old upload, but never to go on under
        // the new name.
        EXPECT_TRUE(std::ranges::none_of(
            remote_->cancels | std::views::values,
            [](FileTracking::PartialFile partial)
            {
                return partial == FileTracking::PartialFile::Kept;
            }
        ));
        EXPECT_FALSE(lastUploadTo("/srv/project/b.bin")->upload.continuesPartialOf.has_value());
    }

    TEST_F(TrackerTest, AFailedServerRenameUploadsTheNewName)
    {
        const auto file = root_ / "local" / "project" / "src" / "main.cpp";
        const auto renamed = root_ / "local" / "project" / "src" / "start.cpp";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();

        std::filesystem::rename(file, renamed);
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return !remote_->of("rename").empty();
            }
        ));
        // Something unknown to the tracker is in the way on the server.
        end(*remote_->of("rename").front(), SharedData::OperationCompletionReason::Failed);

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/src/start.cpp") == 1u;
            }
        ));
        auto const* upload = lastUploadTo("/srv/project/src/start.cpp");
        ASSERT_TRUE(upload->upload.expectedDestination.has_value());
        EXPECT_FALSE(upload->upload.expectedDestination->file.has_value());
    }

    TEST_F(TrackerTest, AFileRenamedAndEditedWhileItUploadsGoesAnewUnderTheNewName)
    {
        const auto file = root_ / "local" / "project" / "a.bin";
        const auto renamed = root_ / "local" / "project" / "b.bin";
        ASSERT_TRUE(tracker_->uploadAndWatch(local(root_ / "local" / "project"), remote("/srv/project")).has_value());
        completeEverything();
        write(file, "large content");
        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/a.bin") == 1u;
            }
        ));

        std::filesystem::rename(file, renamed);
        ASSERT_TRUE(runUntilStopped());
        write(renamed, "edited in the middle");
        runFor(std::chrono::milliseconds{1000});
        endStoppedUploads();

        ASSERT_TRUE(runUntil(
            [this]()
            {
                return uploadsTo("/srv/project/b.bin") == 1u;
            }
        ));
        // Its old part must not be continued, the resume compares only the end of it.
        EXPECT_FALSE(lastUploadTo("/srv/project/b.bin")->upload.continuesPartialOf.has_value());
        ASSERT_EQ(remote_->of("discard").size(), 1u);
        EXPECT_EQ(remote_->of("discard").front()->source, "/srv/project/a.bin");
        completeEverything();
        EXPECT_EQ(remote_->server.at("/srv/project/b.bin"), stateOf(renamed));
    }
}
