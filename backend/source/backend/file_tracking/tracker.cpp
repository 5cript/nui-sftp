#include <backend/file_tracking/tracker.hpp>
#include <backend/file_tracking/conflict_list.hpp>
#include <backend/file_tracking/disk_place.hpp>
#include <backend/file_tracking/failure_list.hpp>
#include <backend/file_tracking/failure_reason.hpp>
#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/local_watch_manager.hpp>
#include <backend/file_tracking/tracking_state.hpp>
#include <backend/file_tracking/watch_progress_meter.hpp>
#include <utility/checked_symlink_status.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/error_message.hpp>
#include <utility/localized_message.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>
#include <shared_data/file_tracking/watch_stop.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief A watch of a LocalWatchManager that ends with this handle.
         */
        class LocalWatchLease
        {
          public:
            LocalWatchLease(LocalWatchManager& manager, Ids::WatchId watchId)
                : manager_{&manager}
                , watchId_{std::move(watchId)}
            {}
            ~LocalWatchLease()
            {
                end();
            }
            LocalWatchLease(LocalWatchLease const&) = delete;
            LocalWatchLease& operator=(LocalWatchLease const&) = delete;
            LocalWatchLease(LocalWatchLease&& other) noexcept
                : manager_{std::exchange(other.manager_, nullptr)}
                , watchId_{std::move(other.watchId_)}
            {}
            LocalWatchLease& operator=(LocalWatchLease&& other) noexcept
            {
                if (this != &other)
                {
                    end();
                    manager_ = std::exchange(other.manager_, nullptr);
                    watchId_ = std::move(other.watchId_);
                }
                return *this;
            }

          private:
            void end()
            {
                if (manager_)
                    manager_->unwatch(watchId_);
            }

          private:
            LocalWatchManager* manager_;
            Ids::WatchId watchId_;
        };

        /**
         * @brief The copy of a download in an instance directory of its own.
         */
        struct DownloadedCopy
        {
            Ids::InstanceId instanceId;
            /** @brief Canonical, like the paths of the changes. */
            std::filesystem::path instanceDirectory;
            InstanceWatch watch;
            /** @brief Runs while the copy was reported deleted or renamed away, see awaitCopyReturn(). */
            std::unique_ptr<boost::asio::steady_timer> returnWait{};
        };

        /**
         * @brief An original the user uploaded.
         */
        struct UploadedOriginal
        {
            LocalWatchLease watch;
        };

        /**
         * @brief One watched local file or directory.
         */
        struct Entry
        {
            Ids::WatchId watchId;
            std::variant<DownloadedCopy, UploadedOriginal> source;
            Paths::RemotePath remotePath;
            /** @brief Stands for remotePath, whatever either is called. */
            Paths::LocalPath localPath;
            SharedData::WatchOptions options;
            PendingUploads pendingUploads{};
            KnownServerContent serverContent{};
            /** @brief Renames and deletes on the server that are queued and have not ended. */
            std::size_t remoteChangesOnTheirWay{0};
            /** @brief What the view shows, so it hears only of changes. */
            bool shownUploading{false};
            /** @brief How far its changes are, by the local paths its uploads started with. */
            WatchProgressMeter progress;
            /** @brief When the view last heard of the progress, it hears of it once per progress interval at most. */
            std::chrono::steady_clock::time_point progressShownAt{};

            DownloadedCopy const* downloadedCopy() const
            {
                return std::get_if<DownloadedCopy>(&source);
            }

            DownloadedCopy* downloadedCopy()
            {
                return std::get_if<DownloadedCopy>(&source);
            }
        };

        /**
         * @brief A changed path of an entry on its way to the server.
         */
        struct Upload
        {
            Ids::WatchId watchId;
            LocalChange change;
            Paths::RemotePath destination;
            /** @brief Compare with the server first, when the entry asks for that. */
            bool checkServer{true};
            /** @brief Refresh the remote listing afterwards, for the upload that starts an "Upload and Watch". */
            bool refreshListing{false};
            /** @brief Its place in the server order once it started, see ServerOperationOrder. */
            std::optional<ServerOperationOrder::Ticket> ticket{};
            /**
             * @brief The destination of a stopped upload of the same file under its old name, whose partial file this
             *        one continues, see UploadRequest::continuesPartialOf.
             */
            std::optional<Paths::RemotePath> continuesPartialOf{};
        };

        /**
         * @brief How the upload of a change ended.
         */
        enum class UploadOutcome
        {
            Uploaded,
            NotUploaded,
            /** @brief The user kept the server's version. */
            Declined,
        };

        /**
         * @brief Whether a watched directory found gone when its watch lost changes went entry by entry. On Windows
         *        only a deletion ends the read of a watch, one moved away is followed. Elsewhere it may have left as a
         *        whole, to the trash, and keeps its server copy like any directory that left with its content. The
         *        removal of the item itself tells more, see wasDeletedWithItsContent.
         */
#ifdef _WIN32
        constexpr bool goneWatchedDirectoryWasDeleted = true;
#else
        constexpr bool goneWatchedDirectoryWasDeleted = false;
#endif

        /**
         * @brief What is at @p path itself: its status, nothing when it is gone, an error when that cannot be told.
         *        Only a path that is gone counts as deleted, see Utility::checkedSymlinkStatus.
         */
        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        localStatusOf(Paths::LocalPath const& path)
        {
            return Utility::checkedSymlinkStatus(path.native());
        }

        bool isGoneLocally(Paths::LocalPath const& path)
        {
            const auto status = localStatusOf(path);
            return status && !status->has_value();
        }

        bool isSymlink(Paths::LocalPath const& path)
        {
            const auto status = localStatusOf(path);
            return status && *status && std::filesystem::is_symlink(**status);
        }

        /**
         * @brief Whether @p path is a directory itself, not a symbolic link to one, which goes up as a link.
         */
        bool isDirectoryLocally(Paths::LocalPath const& path)
        {
            const auto status = localStatusOf(path);
            return status && *status && std::filesystem::is_directory(**status);
        }

        std::uint64_t secondsSinceEpochOf(std::filesystem::file_time_type time)
        {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::file_clock::to_sys(time).time_since_epoch()
            );
            return static_cast<std::uint64_t>(seconds.count());
        }

        std::uint64_t secondsSinceEpochNow()
        {
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
            return static_cast<std::uint64_t>(seconds.count());
        }

        SharedData::WatchStop watchStopOf(SharedData::WatchStopReason reason)
        {
            return {.reason = reason, .stoppedAt = secondsSinceEpochNow()};
        }

        /**
         * @brief What the server has of a file an upload sent in @p sent: uploads carry the modification time along,
         *        in seconds.
         */
        ServerState serverStateOf(SourceState const& sent)
        {
            return ServerState{.mtime = secondsSinceEpochOf(sent.modified), .size = sent.size};
        }

        /**
         * @brief The size of the regular file at @p path, nothing for anything else.
         */
        std::optional<std::uint64_t> fileSizeOf(Paths::LocalPath const& path)
        {
            return sourceStateOf(path.native())
                .transform(
                    [](SourceState const& state)
                    {
                        return state.size;
                    }
                );
        }

        /**
         * @brief How the upload that ended as @p ended counts for the progress: one that found the server's copy
         *        changed did not complete.
         */
        WatchProgressMeter::UploadResult uploadResultOf(UploadEnd const& ended)
        {
            const bool completed =
                ended.reason == SharedData::OperationCompletionReason::Completed && !ended.changedOnServer;
            return completed ? WatchProgressMeter::UploadResult::Completed
                             : WatchProgressMeter::UploadResult::NotCompleted;
        }

        /**
         * @brief What the server has at the destination of an upload instead of what the tracker left there.
         */
        struct ServerChange
        {
            SharedData::ConflictKind kind{SharedData::ConflictKind::ChangedOnServer};
            /** @brief What it has there now, nothing when it has nothing there or could not be asked. */
            std::optional<ServerState> current{};
        };

        /**
         * @brief How the server changed where the tracker knew of something there (@p knownThere) or of nothing, and
         *        it has @p current now. Changed when that cannot be told.
         */
        ServerChange serverChangeOf(bool knownThere, std::optional<ServerState> const& current)
        {
            if (!knownThere && current)
                return {.kind = SharedData::ConflictKind::CreatedOnServer, .current = current};
            if (knownThere && !current)
                return {.kind = SharedData::ConflictKind::DeletedOnServer, .current = std::nullopt};
            return {.kind = SharedData::ConflictKind::ChangedOnServer, .current = current};
        }

        /**
         * @brief The conflict @p upload raised, the server changed its destination as @p change tells. Its id is set
         *        when it is listed.
         */
        SharedData::WatchConflict conflictOf(Upload const& upload, ServerChange const& change)
        {
            const auto localState = sourceStateOf(upload.change.path.native());
            return {
                .conflictId = {},
                .watchId = upload.watchId,
                .kind = change.kind,
                .localPath = upload.change.path,
                .remotePath = upload.destination,
                .serverModifiedAt = change.current ? std::optional{change.current->mtime} : std::nullopt,
                .localModifiedAt = localState ? std::optional{secondsSinceEpochOf(localState->modified)} : std::nullopt,
                .raisedAt = secondsSinceEpochNow(),
            };
        }

        /**
         * @brief The remote path of another watch, or of one that starts, that a remote path overlaps.
         */
        struct RemoteOverlap
        {
            Paths::RemotePath remotePath;
            /** @brief Nothing for a watch that starts, see Implementation::claimRemote(). */
            std::optional<Ids::WatchId> watchId{};
            /** @brief The name of its item, nothing for a watch that starts. */
            std::optional<std::string> watchName{};
        };

        /**
         * @brief Whether @p option may be on for a downloaded copy: removing or renaming a temporary copy never touches
         *        the server's file.
         */
        bool appliesToDownloadedCopies(SharedData::WatchOption option)
        {
            return option != SharedData::WatchOption::DeleteRemoteOnLocalDelete &&
                option != SharedData::WatchOption::MoveRemoteOnLocalMove;
        }

        SharedData::WatchOptions downloadedCopyOptions(SharedData::WatchOptions options)
        {
            options.deleteRemoteOnLocalDelete = false;
            options.moveRemoteOnLocalMove = false;
            return options;
        }

        std::string shownNameOf(Paths::LocalPath const& path)
        {
            return path.filename() ? std::string{path.filename()->view()} : path.toUtf8();
        }

        SharedData::TrackedEntry shownEntryOf(Entry const& entry)
        {
            return SharedData::TrackedEntry{
                .watchId = entry.watchId,
                .name = shownNameOf(entry.localPath),
                .remotePath = entry.remotePath,
                .options = entry.options,
                .uploading = entry.shownUploading,
                .downloadedCopy = entry.downloadedCopy() != nullptr,
                .localPath = entry.localPath,
                .isDirectory = entry.downloadedCopy() == nullptr && isDirectoryLocally(entry.localPath),
                .stop = std::nullopt,
                .instanceId = entry.downloadedCopy() ? std::optional{entry.downloadedCopy()->instanceId} : std::nullopt,
            };
        }

        std::expected<Paths::RemotePath, Paths::PathError>
        remotePathOf(Entry const& entry, Paths::LocalPath const& local)
        {
            return remotePathOfChange(local, entry.localPath, entry.remotePath);
        }

        /**
         * @brief The remote path of @p remotePath's item when it is renamed like the local one to @p newPath.
         */
        std::expected<Paths::RemotePath, std::string>
        renamedRemotePath(Paths::RemotePath const& remotePath, Paths::LocalPath const& newPath)
        {
            const auto remoteDirectory = remotePath.parent();
            const auto name = newPath.filename();
            if (!remoteDirectory || !name)
                return std::unexpected{
                    Paths::PathError{.reason = Paths::PathRejection::EscapesRoot, .input = newPath.toUtf8()}.toString()
                };
            return *remoteDirectory / *name;
        }

        /**
         * @brief @p path spelled like the path @p entry knows the server copy under, where the file system ignores
         *        Unicode normalization (macOS): efsw reports every name precomposed (NFC), the server copy has the
         *        spelling its directory listed. Unchanged elsewhere and when nothing known matches.
         */
        Paths::LocalPath knownSpelling([[maybe_unused]] Entry const& entry, Paths::LocalPath const& path)
        {
#ifdef __APPLE__
            return entry.serverContent.knownSpellingOf(path).value_or(path);
#else
            return path;
#endif
        }

        /**
         * @brief @p path as a local path, nothing when it is empty or unusable.
         */
        std::optional<Paths::LocalPath> localPathOf(std::filesystem::path const& path)
        {
            if (path.empty())
                return std::nullopt;
            return Paths::LocalPath::make(path)
                .transform(
                    [](Paths::LocalPath made)
                    {
                        return std::optional{std::move(made)};
                    }
                )
                .value_or(std::nullopt);
        }

        /**
         * @brief Where the download of @p remotePath goes in @p instanceDirectory.
         */
        std::expected<Paths::LocalPath, std::string>
        copyPathOf(std::filesystem::path const& instanceDirectory, Paths::RemotePath const& remotePath)
        {
            const auto name = remotePath.filename();
            if (!name)
                return std::unexpected{
                    Paths::PathError{.reason = Paths::PathRejection::Empty, .input = remotePath.value()}.toString()
                };
            return Paths::LocalPathSegment::make(*name)
                .and_then(
                    [&instanceDirectory](Paths::LocalPathSegment const& segment)
                    {
                        return Paths::LocalPath::make(instanceDirectory)
                            .transform(
                                [&segment](Paths::LocalPath const& directory)
                                {
                                    return directory / segment;
                                }
                            );
                    }
                )
                .transform_error(
                    [](Paths::PathError const& error)
                    {
                        return error.toString();
                    }
                );
        }
    }

    struct Tracker::Implementation : public std::enable_shared_from_this<Tracker::Implementation>
    {
        /** @brief How often the view hears of the progress of a watch at most. */
        constexpr static std::chrono::milliseconds progressInterval{200};

        /**
         * @brief Holds the remote path of a watch that starts, see claimRemote(), until it is released or goes.
         */
        class Claim
        {
          public:
            Claim(std::weak_ptr<Implementation> tracker, std::uint64_t claimId)
                : tracker_{std::move(tracker)}
                , claimId_{claimId}
            {}
            ~Claim()
            {
                release();
            }
            Claim(Claim const&) = delete;
            Claim& operator=(Claim const&) = delete;
            Claim(Claim&&) = delete;
            Claim& operator=(Claim&&) = delete;

            void release()
            {
                if (const auto tracker = std::exchange(tracker_, {}).lock())
                    tracker->claims.erase(claimId_);
            }

          private:
            std::weak_ptr<Implementation> tracker_;
            std::uint64_t claimId_;
        };

        /**
         * @brief The ids of the conflicts and failures the view shows of watches.
         */
        struct ShownProblems
        {
            std::vector<Ids::ConflictId> conflicts{};
            std::vector<Ids::FailureId> failures{};
        };

        using OnOpened = std::function<void(std::expected<OpenedCopy, OpenFailure> const&)>;

        boost::asio::strand<boost::asio::any_io_executor> strand;
        TempDirInstanceManager* instances;
        LocalWatchManager* localWatches;
        std::unique_ptr<TrackerRemote> remote;
        std::unique_ptr<TrackerView> view;
        std::string downloadTempSuffix;
        std::function<SharedData::WatchOptions()> defaultOptions;

        std::map<Ids::WatchId, Entry> entries{};
        /** @brief The watches that stopped, as the view shows them until they are removed, see retire(). */
        std::map<Ids::WatchId, SharedData::TrackedEntry> stopped{};
        /** @brief The instances of downloads on their way, see open(). */
        std::set<Ids::InstanceId> downloading{};
        /** @brief The instances of copies opened without a watch, they end with the tracker, see stopAll(). */
        std::set<Ids::InstanceId> openedCopies{};
        /** @brief The remote paths of watches that are starting, by claim. */
        std::map<std::uint64_t, Paths::RemotePath> claims{};
        std::uint64_t nextClaimId{0};
        /** @brief The conflicts waiting for the user's answer, each with the upload that waits for it. */
        ConflictList<Upload> conflicts{};
        /** @brief The failed uploads waiting for Retry or Dismiss. */
        FailureList failures{};
        /** @brief Shows the progress that changed, see showDueProgress(). */
        boost::asio::steady_timer progressTimer;
        bool progressScheduled{false};
        /** @brief The idle entries are shown once the current handler ended, see showIdleProgress(). */
        bool idleProgressPosted{false};
        ServerOperationOrder serverOrder{};
        /** @brief Start the operations that wait in serverOrder, by ticket. */
        std::map<ServerOperationOrder::Ticket, std::function<void(ServerOperationOrder::Ticket)>> waitingOperations{};

        Implementation(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            TempDirInstanceManager& instances,
            LocalWatchManager& localWatches,
            std::unique_ptr<TrackerRemote> remote,
            std::unique_ptr<TrackerView> view,
            std::string downloadTempSuffix,
            std::function<SharedData::WatchOptions()> defaultOptions
        )
            : strand{std::move(strand)}
            , instances{&instances}
            , localWatches{&localWatches}
            , remote{std::move(remote)}
            , view{std::move(view)}
            , downloadTempSuffix{std::move(downloadTempSuffix)}
            , defaultOptions{std::move(defaultOptions)}
            , progressTimer{this->strand}
        {}

        Entry* find(Ids::WatchId const& watchId)
        {
            const auto found = entries.find(watchId);
            return found == entries.end() ? nullptr : &found->second;
        }

        /**
         * @brief Hands the changes of the watch @p watchId to the strand, the watch may end while one is handled.
         */
        ChangeSink sinkOf(Ids::WatchId const& watchId)
        {
            return [strand = strand, weak = weak_from_this(), watchId](ReportedChange const& change)
            {
                boost::asio::post(
                    strand,
                    [weak, watchId, change]()
                    {
                        if (const auto self = weak.lock())
                            self->onChange(watchId, change);
                    }
                );
            };
        }

        /**
         * @brief Runs @p start once the server operations decided before it on @p paths ended, see
         *        ServerOperationOrder. It gets its ticket, leaveOrder() ends it.
         */
        void inOrder(std::vector<Paths::RemotePath> paths, std::function<void(ServerOperationOrder::Ticket)> start)
        {
            const auto [ticket, startsNow] = serverOrder.enter(std::move(paths));
            if (startsNow)
                return start(ticket);
            waitingOperations.emplace(ticket, std::move(start));
        }

        void leaveOrder(ServerOperationOrder::Ticket ticket)
        {
            for (const auto starting : serverOrder.leave(ticket))
            {
                auto node = waitingOperations.extract(starting);
                // Dropped by stopAll(), it never starts.
                if (node.empty())
                    leaveOrder(starting);
                else
                    node.mapped()(starting);
            }
        }

        void leaveOrder(Upload const& upload)
        {
            if (upload.ticket)
                leaveOrder(*upload.ticket);
        }

        /**
         * @brief Ends @p ticket once the queued operation ended.
         */
        TrackerRemote::OnEnded leavesOrderWhenEnded(ServerOperationOrder::Ticket ticket)
        {
            return [weak = weak_from_this(), ticket](SharedData::OperationCompletionReason)
            {
                if (const auto self = weak.lock())
                    self->leaveOrder(ticket);
            };
        }

        /**
         * @brief Hands the end of the queued operation to @p ended, then ends @p ticket.
         */
        TrackerRemote::OnEnded endsAndLeavesOrder(TrackerRemote::OnEnded ended, ServerOperationOrder::Ticket ticket)
        {
            return [ended = std::move(ended), leaves = leavesOrderWhenEnded(ticket)](
                       SharedData::OperationCompletionReason reason
                   )
            {
                ended(reason);
                leaves(reason);
            };
        }

        void show(Entry const& entry)
        {
            view->showEntry(shownEntryOf(entry));
        }

        void showProgress(Entry& entry, std::chrono::steady_clock::time_point now)
        {
            entry.progress.takeDirty();
            entry.progressShownAt = now;
            view->showWatchProgress(entry.progress.snapshot());
        }

        /**
         * @brief Shows the progress of @p entry when it changed, at most once per progress interval. An entry with
         *        nothing on its way ends its burst first.
         * @return When it may be shown, nothing when it was shown or did not change.
         */
        std::optional<std::chrono::steady_clock::time_point>
        showChangedProgress(Entry& entry, std::chrono::steady_clock::time_point now)
        {
            if (entry.progress.isIdle())
                entry.progress.finishBurst(secondsSinceEpochNow());
            if (!entry.progress.isDirty())
                return std::nullopt;
            if (const auto due = entry.progressShownAt + progressInterval; now < due)
                return due;
            showProgress(entry, now);
            return std::nullopt;
        }

        /**
         * @brief The counts of @p entry may have changed or its uploads went on: the view hears of it once the
         *        progress interval passed, or right after the change that left nothing on its way, see
         *        showIdleProgress().
         */
        void progressChanged(Entry& entry)
        {
            entry.progress.setChangesPending(entry.pendingUploads.changeCount() + entry.remoteChangesOnTheirWay);
            entry.progress.setWaitingForAnswer(conflicts.countOf(entry.watchId));
            if (!entry.progress.isDirty())
                return;
            if (entry.progress.isIdle())
                return postIdleProgress();
            scheduleProgress(std::chrono::steady_clock::now() + progressInterval);
        }

        void progressChanged(Ids::WatchId const& watchId)
        {
            if (auto* entry = find(watchId))
                progressChanged(*entry);
        }

        void postIdleProgress()
        {
            if (std::exchange(idleProgressPosted, true))
                return;
            // Posted, so the changes the end of an upload starts are on their way by then.
            boost::asio::post(
                strand,
                [weak = weak_from_this()]()
                {
                    if (const auto self = weak.lock())
                        self->showIdleProgress();
                }
            );
        }

        /**
         * @brief The entries with nothing on their way end their burst. The view hears of them at once, or once the
         *        progress interval since it last did passed.
         */
        void showIdleProgress()
        {
            idleProgressPosted = false;
            const auto now = std::chrono::steady_clock::now();
            for (auto& entry : std::views::values(entries))
            {
                if (!entry.progress.isIdle())
                    continue;
                if (const auto due = showChangedProgress(entry, now))
                    scheduleProgress(*due);
            }
        }

        void scheduleProgress(std::chrono::steady_clock::time_point at)
        {
            if (std::exchange(progressScheduled, true))
                return;
            progressTimer.expires_at(at);
            progressTimer.async_wait(
                [weak = weak_from_this()](boost::system::error_code const& error)
                {
                    if (error)
                        return;
                    if (const auto self = weak.lock())
                        self->showDueProgress();
                }
            );
        }

        /**
         * @brief Shows the progress that changed, the timer runs again for that of entries shown too recently.
         */
        void showDueProgress()
        {
            progressScheduled = false;
            const auto now = std::chrono::steady_clock::now();
            std::optional<std::chrono::steady_clock::time_point> next;
            for (auto& entry : std::views::values(entries))
                if (const auto due = showChangedProgress(entry, now); due && (!next || *due < *next))
                    next = due;
            if (next)
                scheduleProgress(*next);
        }

        /**
         * @param count The changes it counted as, see PendingUploads::changeCount().
         */
        void changeEnded(Entry& entry, std::size_t count = 1)
        {
            entry.progress.changesEnded(count);
            progressChanged(entry);
        }

        /**
         * @brief The upload of @p path in the watch @p watchId, the path it started with, is as far as @p progress.
         */
        void onUploadProgress(Ids::WatchId const& watchId, Paths::LocalPath const& path, UploadProgress const& progress)
        {
            auto* entry = find(watchId);
            if (!entry)
                return;
            entry->progress.uploadProgressed(path, progress);
            progressChanged(*entry);
        }

        /**
         * @brief Hears when a rename or delete on the server for @p entry ended, it is on its way until then.
         */
        TrackerRemote::OnEnded remoteChangeOf(Entry& entry)
        {
            ++entry.remoteChangesOnTheirWay;
            progressChanged(entry);
            return [weak = weak_from_this(), watchId = entry.watchId](SharedData::OperationCompletionReason)
            {
                const auto self = weak.lock();
                if (!self)
                    return;
                auto* ended = self->find(watchId);
                if (!ended)
                    return;
                if (ended->remoteChangesOnTheirWay > 0)
                    --ended->remoteChangesOnTheirWay;
                self->changeEnded(*ended);
            };
        }

        void showUploading(Entry& entry)
        {
            const bool uploading = !entry.pendingUploads.empty();
            if (uploading == entry.shownUploading)
                return;
            entry.shownUploading = uploading;
            show(entry);
        }

        /**
         * @brief Refuses @p remotePath when it overlaps the remote path of a watch other than @p ignoredWatchId or of
         *        a claim: it is the same, lies below it or holds it.
         */
        std::expected<void, std::string> refuseRemoteOverlap(
            Paths::RemotePath const& remotePath,
            std::optional<Ids::WatchId> const& ignoredWatchId = std::nullopt
        ) const
        {
            const auto overlap = remoteOverlapOf(remotePath, ignoredWatchId);
            if (!overlap)
                return {};
            return std::unexpected{Utility::localizedMessage(
                "backend.fileTracking.remoteOverlaps", remotePath.value(), overlap->remotePath.value()
            )};
        }

        /**
         * @brief What @p remotePath overlaps, see refuseRemoteOverlap(). Stopped watches claim nothing.
         */
        std::optional<RemoteOverlap> remoteOverlapOf(
            Paths::RemotePath const& remotePath,
            std::optional<Ids::WatchId> const& ignoredWatchId = std::nullopt
        ) const
        {
            const auto overlaps = [&remotePath](Paths::RemotePath const& other)
            {
                return remotePath.isWithin(other) || other.isWithin(remotePath);
            };
            for (auto const& entry : std::views::values(entries))
                if (entry.watchId != ignoredWatchId && overlaps(entry.remotePath))
                    return RemoteOverlap{
                        .remotePath = entry.remotePath,
                        .watchId = entry.watchId,
                        .watchName = shownNameOf(entry.localPath),
                    };
            for (auto const& claimed : std::views::values(claims))
                if (overlaps(claimed))
                    return RemoteOverlap{.remotePath = claimed};
            return std::nullopt;
        }

        std::expected<std::shared_ptr<Claim>, std::string> claimRemote(Paths::RemotePath const& remotePath)
        {
            return refuseRemoteOverlap(remotePath)
                .transform(
                    [this, &remotePath]()
                    {
                        const auto claimId = nextClaimId++;
                        claims.emplace(claimId, remotePath);
                        return std::make_shared<Claim>(weak_from_this(), claimId);
                    }
                );
        }

        /**
         * @brief Shows @p entry with its progress and mirrors its changes from now on.
         */
        Entry& track(Entry entry)
        {
            const auto watchId = entry.watchId;
            auto& tracked = entries.insert_or_assign(watchId, std::move(entry)).first->second;
            show(tracked);
            showProgress(tracked, std::chrono::steady_clock::now());
            return tracked;
        }

        void open(Paths::RemotePath const& remotePath, std::uint64_t sizeBytes, bool watch, OnOpened onOpened)
        {
            auto claim = std::shared_ptr<Claim>{};
            if (watch)
            {
                auto claimed = claimRemote(remotePath);
                if (!claimed)
                    return onOpened(std::unexpected{OpenFailure{claimed.error()}});
                claim = *std::move(claimed);
            }
            auto* instance = instances->createInstance();
            if (!instance)
                return onOpened(std::unexpected{
                    OpenFailure{Utility::localizedMessage("backend.fileTracking.instanceCreationFailed")}
                });
            const auto instanceId = Ids::makeInstanceId(instance->instanceId());
            // Only the orphan list shows it, the copy opens without it.
            if (const auto recorded = instance->recordOrigin(remotePath); !recorded)
                Log::warn("FileTracking: {}", recorded.error());
            downloading.insert(instanceId);
            // Canonical like the paths of the changes, which the copy's path is compared with.
            std::error_code canonicalError;
            const auto canonicalDirectory = std::filesystem::weakly_canonical(instance->instanceDir(), canonicalError);
            const auto instanceDirectory = canonicalError ? instance->instanceDir() : canonicalDirectory;
            const auto localPath = copyPathOf(instanceDirectory, remotePath);
            const auto queued = localPath.and_then(
                [&, this](Paths::LocalPath const& destination)
                {
                    return remote->download(
                        remotePath,
                        destination,
                        sizeBytes,
                        [weak = weak_from_this(),
                            copy = OpenedCopy{.instanceId = instanceId, .localPath = destination},
                            instanceDirectory,
                            remotePath,
                            watch,
                            claim,
                            onOpened](SharedData::OperationCompletionReason reason)
                        {
                            // The watch is tracked or refused by the time anything else runs on the strand.
                            if (claim)
                                claim->release();
                            if (const auto self = weak.lock())
                                self->onDownloaded(copy, instanceDirectory, remotePath, watch, reason, onOpened);
                        }
                    );
                }
            );
            if (queued)
                return;
            // The caller shows why, an error would show it a second time.
            Log::warn("FileTracking: not downloading '{}': {}", remotePath.value(), queued.error());
            downloading.erase(instanceId);
            instances->removeInstance(instanceId.value());
            onOpened(std::unexpected{OpenFailure{queued.error()}});
        }

        void onDownloaded(
            OpenedCopy const& copy,
            std::filesystem::path const& instanceDirectory,
            Paths::RemotePath const& remotePath,
            bool watch,
            SharedData::OperationCompletionReason reason,
            OnOpened const& onOpened
        )
        {
            // Dropped by stopAll() meanwhile, with its instance.
            if (downloading.erase(copy.instanceId) == 0)
                return onOpened(std::unexpected{OpenFailure{}});
            if (reason != SharedData::OperationCompletionReason::Completed)
            {
                Log::warn("FileTracking: the download of '{}' did not complete", remotePath.value());
                instances->removeInstance(copy.instanceId.value());
                return onOpened(std::unexpected{OpenFailure{}});
            }
            if (!watch)
            {
                openedCopies.insert(copy.instanceId);
                return onOpened(copy);
            }
            auto* instance = instances->findInstance(copy.instanceId.value());
            const auto watchId = Ids::generateWatchId();
            auto instanceWatch = instance
                ? instance->addWatch(instanceDirectory, /*recursive=*/true, downloadTempSuffix, sinkOf(watchId))
                : std::nullopt;
            if (!instanceWatch)
            {
                Log::error("FileTracking: cannot watch the download '{}'", copy.localPath.toUtf8());
                instances->removeInstance(copy.instanceId.value());
                return onOpened(std::unexpected{OpenFailure{
                    Utility::localizedMessage("backend.fileTracking.watchCopyFailed", copy.localPath.toUtf8())
                }});
            }
            Log::info(
                "FileTracking: watching {} of instance {} as {}",
                copy.localPath.toUtf8(),
                copy.instanceId.value(),
                watchId.value()
            );
            auto& entry = track(Entry{
                .watchId = watchId,
                .source =
                    DownloadedCopy{
                        .instanceId = copy.instanceId,
                        .instanceDirectory = instanceDirectory,
                        .watch = *std::move(instanceWatch),
                    },
                .remotePath = remotePath,
                .localPath = copy.localPath,
                .options = downloadedCopyOptions(defaultOptions()),
                .progress = WatchProgressMeter{watchId},
            });
            // The downloaded files carry the server's modification time, so they tell what the server had.
            rememberStates(entry, std::nullopt);
            onOpened(copy);
        }

        std::expected<UploadTarget, std::string>
        checkUploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& directory) const
        {
            return localWatches->checkWatchable(item.native())
                .and_then(
                    [](std::filesystem::path const& canonical)
                    {
                        return Paths::LocalPath::make(canonical)
                            .transform_error(
                                [](Paths::PathError const& error)
                                {
                                    return error.toString();
                                }
                            );
                    }
                )
                .and_then(
                    [this, &directory](Paths::LocalPath const& canonical) -> std::expected<UploadTarget, std::string>
                    {
                        const auto name = canonical.filename();
                        if (!name)
                            return std::unexpected{
                                Paths::PathError{.reason = Paths::PathRejection::Empty, .input = canonical.toUtf8()}
                                    .toString()
                            };
                        const auto target = UploadTarget{.item = canonical, .destination = directory / *name};
                        return refuseRemoteOverlap(target.destination)
                            .transform(
                                [&target]()
                                {
                                    return target;
                                }
                            );
                    }
                );
        }

        std::expected<void, std::string>
        uploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& destination)
        {
            Log::info("FileTracking: upload and watch {} to {}", item.toUtf8(), destination.value());
            const auto watchId = Ids::generateWatchId();
            return refuseRemoteOverlap(destination)
                .and_then(
                    [&, this]()
                    {
                        return localWatches->watch(watchId, item.native(), downloadTempSuffix, sinkOf(watchId));
                    }
                )
                .and_then(
                    [&, this](LocalItemWatch const* watched) -> std::expected<void, std::string>
                    {
                        auto lease = LocalWatchLease{*localWatches, watchId};
                        const auto watchedItem = Paths::LocalPath::make(watched->item());
                        if (!watchedItem)
                            return std::unexpected{watchedItem.error().toString()};
                        const bool isDirectory = watched->isDirectory();
                        Log::info("FileTracking: watching {} as {}", watchedItem->toUtf8(), watchId.value());
                        track(Entry{
                            .watchId = watchId,
                            .source = UploadedOriginal{.watch = std::move(lease)},
                            .remotePath = destination,
                            .localPath = *watchedItem,
                            .options = defaultOptions(),
                            .progress = WatchProgressMeter{watchId},
                        });
                        // Goes like a change, so what changes while it uploads goes once more afterwards.
                        queueUpload(Upload{
                            .watchId = watchId,
                            .change = {.path = *watchedItem, .isDirectory = isDirectory},
                            .destination = destination,
                            .checkServer = false,
                            .refreshListing = true,
                        });
                        return {};
                    }
                );
        }

        void stop(Ids::WatchId const& watchId)
        {
            if (retire(watchId, watchStopOf(SharedData::WatchStopReason::StoppedByUser)))
                showOrphanedCopies();
        }

        /**
         * @brief Ends the watch @p watchId like forget(), but the view keeps showing it as stopped by @p stop until
         *        removeStopped(). A downloaded copy's instance ends, and the watch claims no remote path any more.
         * @return The stopped entry as the view shows it, nothing when there is no such watch.
         */
        std::optional<SharedData::TrackedEntry> retire(Ids::WatchId watchId, SharedData::WatchStop stop)
        {
            if (!entries.contains(watchId))
                return std::nullopt;
            Log::info("FileTracking: stopping watch {}: {}", watchId.value(), Utility::enumToString(stop.reason));
            auto shown = endWatch(watchId, stop.stoppedAt);
            shown.stop = std::move(stop);
            if (shown.instanceId)
                releaseInstance(*shown.instanceId);
            view->showEntry(shown);
            stopped.insert_or_assign(watchId, shown);
            return shown;
        }

        /**
         * @brief Ends the watch @p watchId, which exists: its conflicts and failures leave the view, which hears of its
         *        last progress, its burst ends at @p endedAt. What is on its way goes on unshown.
         * @return The entry as the view shows it once it ended.
         */
        SharedData::TrackedEntry endWatch(Ids::WatchId const& watchId, std::uint64_t endedAt)
        {
            auto node = entries.extract(watchId);
            auto& entry = node.mapped();
            dropProblems(watchId);
            entry.progress.dropPending();
            entry.progress.finishBurst(endedAt);
            view->showWatchProgress(entry.progress.snapshot());
            auto shown = shownEntryOf(entry);
            shown.uploading = false;
            // The item may be gone or elsewhere by now, its watch knows what it was.
            if (auto const* const watch = localWatches->find(watchId))
                shown.isDirectory = watch->isDirectory();
            return shown;
        }

        /**
         * @brief Ends the instance of a copy that is no longer watched or opened: it counts as orphaned then, or goes
         *        with its directory when nothing is left in it.
         */
        void releaseInstance(Ids::InstanceId const& instanceId)
        {
            const auto* instance = instances->findInstance(instanceId.value());
            if (!instance)
                return;
            if (InstanceManagerDetail::hasContent(instance->instanceDir()))
                instances->destroyInstance(instanceId.value());
            else
                instances->removeInstance(instanceId.value());
        }

        /**
         * @brief Ends the watch @p watchId and takes its entry off the view. The instance of a downloaded copy stays.
         */
        void forget(Ids::WatchId const& watchId)
        {
            auto node = entries.extract(watchId);
            if (node.empty())
                return;
            Log::info("FileTracking: forgetting watch {}", watchId.value());
            dropProblems(watchId);
            view->removeEntry(watchId);
        }

        /**
         * @brief Drops the conflicts and failures of the watch @p watchId and takes them off the view, conflicts
         *        first. The uploads that waited for an answer leave the server order.
         */
        void dropProblems(Ids::WatchId const& watchId)
        {
            ShownProblems shown;
            collectShownProblems(watchId, shown);
            const auto dropped = conflicts.removeWatch(watchId);
            failures.removeWatch(watchId);
            hideProblems(shown);
            for (auto const& listed : dropped)
                leaveOrder(listed.context);
        }

        void collectShownProblems(Ids::WatchId const& watchId, ShownProblems& shown) const
        {
            for (auto const& conflict : conflicts.conflictsOf(watchId))
                shown.conflicts.push_back(conflict.conflictId);
            for (auto const& failure : failures.failuresOf(watchId))
                shown.failures.push_back(failure.failureId);
        }

        void hideProblems(ShownProblems const& shown)
        {
            for (auto const& conflictId : shown.conflicts)
                view->removeConflict(conflictId);
            for (auto const& failureId : shown.failures)
                view->removeFailure(failureId);
        }

        void setOption(Ids::WatchId const& watchId, SharedData::WatchOption option, bool value)
        {
            auto* entry = find(watchId);
            if (!entry)
                return;
            if (entry->downloadedCopy() && !appliesToDownloadedCopies(option))
                Log::warn(
                    "FileTracking: watch {} is a downloaded copy, {} stays off",
                    watchId.value(),
                    Utility::enumToString(option)
                );
            else
                entry->options[option] = value;
            // Also when nothing changed, so a switch the user flipped shows the option again.
            show(*entry);
        }

        std::expected<DiscardOutcome, std::string>
        discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& path)
        {
            Log::info("FileTracking: discarding {} of instance {}", path.toUtf8(), instanceId.value());
            const auto watched = std::ranges::find_if(
                entries,
                [&instanceId](auto const& entry)
                {
                    const auto* copy = entry.second.downloadedCopy();
                    return copy && copy->instanceId == instanceId;
                }
            );
            if (watched != entries.end())
                forget(watched->first);
            const auto discarded = instances->discardContent(instanceId.value(), path.native());
            if (discarded == DiscardOutcome::InstanceDestroyed)
                openedCopies.erase(instanceId);
            return discarded;
        }

        void stopAll()
        {
            endEverything();
            stopped.clear();
        }

        /**
         * @brief Ends every watch, opened copy and download without telling the view, see Tracker::stopAll(). The
         *        stopped watches stay.
         */
        void endEverything()
        {
            auto copies = std::exchange(openedCopies, {});
            copies.merge(std::exchange(downloading, {}));
            for (auto const& entry : std::views::values(entries))
                if (const auto* copy = entry.downloadedCopy())
                    copies.insert(copy->instanceId);
            entries.clear();
            for (auto const& instanceId : copies)
                releaseInstance(instanceId);
            claims.clear();
            const auto dropped = conflicts.removeAll();
            failures.removeAll();
            const auto waiting = std::exchange(waitingOperations, {});
            for (auto const& listed : dropped)
                leaveOrder(listed.context);
            for (auto const& ticket : std::views::keys(waiting))
                leaveOrder(ticket);
            progressTimer.cancel();
            progressScheduled = false;
        }

        void connectionLost()
        {
            Log::info("FileTracking: the connection was lost, ending {} watch(es)", entries.size());
            // Nothing that waits in the server order starts any more.
            const auto waiting = std::exchange(waitingOperations, {});
            const auto stop = watchStopOf(SharedData::WatchStopReason::ConnectionLost);
            auto ended = SharedData::EndedWatches{.reason = SharedData::WatchEndReason::ConnectionLost, .entries = {}};
            for (auto const& watchId : std::views::keys(entries) | std::ranges::to<std::vector>())
                if (auto retired = retire(watchId, stop))
                    ended.entries.push_back(*std::move(retired));
            endEverything();
            for (auto const& ticket : std::views::keys(waiting))
                leaveOrder(ticket);
            if (!ended.entries.empty())
                view->showEnded(ended);
            showOrphanedCopies();
        }

        /**
         * @brief Records what the server has after a download or an upload: the local states below @p below, or of
         *        the whole item without it. Transfers carry the modification time along, a symbolic link is asked
         *        for on the server, which may have skipped it.
         * @return How many entries it recorded.
         */
        std::size_t rememberStates(Entry& entry, std::optional<Paths::LocalPath> const& below)
        {
            std::size_t recorded = 0;
            const auto states = [&]()
            {
                if (const auto* copy = entry.downloadedCopy())
                    return InstanceManagerDetail::fileStatesOf(
                        copy->instanceDirectory, below ? below->native() : std::filesystem::path{}
                    );
                return itemStatesOf(below.value_or(entry.localPath).native());
            }();
            for (auto const& state : states)
            {
                const auto path = Paths::LocalPath::make(state.path);
                if (!path || (below && !path->isWithin(*below)))
                    continue;
                if (state.isDirectory)
                    entry.serverContent.rememberDirectory(*path);
                else if (state.isSymlink)
                    rememberLink(entry, *path);
                else
                    entry.serverContent.rememberFile(*path, ServerState{.mtime = state.mtime, .size = state.size});
                ++recorded;
            }
            return recorded;
        }

        /**
         * @brief Records what the server has after the upload of @p directory, see rememberStates(), but for the
         *        entries it left out (@p failedEntries), which keep what the server was known to have of them.
         * @return How many entries it recorded.
         */
        std::size_t rememberUploadedStates(
            Entry& entry,
            Paths::LocalPath const& directory,
            std::vector<FailedUploadEntry> const& failedEntries
        )
        {
            std::vector<std::pair<Paths::LocalPath, std::optional<ServerState>>> known;
            for (auto const& failed : failedEntries)
                if (failed.path != directory && failed.path.isWithin(directory) &&
                    !entry.serverContent.hasDirectory(failed.path))
                    known.emplace_back(failed.path, entry.serverContent.fileState(failed.path));
            const auto recorded = rememberStates(entry, directory);
            for (auto const& [path, state] : known)
            {
                if (state)
                    entry.serverContent.rememberFile(path, *state);
                else
                    entry.serverContent.forget(path);
            }
            return recorded;
        }

        void rememberLink(Entry& entry, Paths::LocalPath const& path)
        {
            const auto remotePath = remotePathOf(entry, path);
            if (!remotePath)
                return;
            if (const auto state = remote->linkStat(*remotePath); state && *state)
                entry.serverContent.rememberFile(path, **state);
        }

        void onChange(Ids::WatchId const& watchId, ReportedChange const& reported)
        {
            auto* entry = find(watchId);
            if (!entry)
                return;
            const auto reportedPath = localPathOf(reported.path);
            if (!reportedPath)
            {
                Log::warn(
                    "FileTracking: ignoring a change in watch {}: '{}'",
                    watchId.value(),
                    Paths::nativeToUtf8(reported.path)
                );
                return;
            }
            const auto oldPath = localPathOf(reported.oldPath).transform(
                [entry](Paths::LocalPath const& old)
                {
                    return knownSpelling(*entry, old);
                }
            );
            const auto respelled = knownSpelling(*entry, *reportedPath);
            // A rename that changes the normalization only keeps its new spelling, the server copy follows it.
            const auto path = reported.action == FileAction::Moved && oldPath == respelled ? *reportedPath : respelled;
            Log::debug(
                "FileTracking: change in watch {}: {} {}",
                watchId.value(),
                Utility::enumToString(reported.action),
                path.toUtf8()
            );
            if (reported.action == FileAction::Missed)
                return onMissed(*entry, path);
            if (reported.action == FileAction::Moved && oldPath == entry->localPath)
            {
                // The local watch follows an original renamed beside itself once it is clear the rename stays.
                if (!entry->downloadedCopy())
                    return onItemMoved(*entry, path);
                // A copy renamed away is not what goes to the server any more.
                if (path != entry->localPath)
                    return awaitCopyReturn(*entry);
            }
            // Only the tracked file or directory maps to the remote side, anything else in its directory (Finder's
            // .DS_Store for one) would land next to it on the server.
            if (!path.isWithin(entry->localPath))
                return;
            // The watch reports a link to a directory as a directory.
            const LocalChange change{.path = path, .isDirectory = reported.isDirectory && !isSymlink(path)};
            if (reported.action == FileAction::Deleted)
                return onDeleted(*entry, change.path);
            // A move from outside the item into it (an atomic save renaming a temporary over it) is a change.
            if (reported.action == FileAction::Moved && oldPath && oldPath->isWithin(entry->localPath))
                return onMoved(*entry, *oldPath, change);
            onContentChanged(*entry, change);
        }

        /**
         * @param checkServer Compare with the server first, when the entry asks for that. Changes below a directory
         *        that was just uploaded skip it, the states recorded after that upload may already be newer.
         */
        void onContentChanged(Entry& entry, LocalChange change, bool checkServer = true)
        {
            if (!entry.options.autoReupload)
                return;
            // A change that waited below a directory upload was reported before that upload told what the server has.
            change.path = knownSpelling(entry, change.path);
            const auto destination = remotePathOf(entry, change.path);
            if (!destination)
            {
                Log::warn(
                    "FileTracking: not uploading the change in watch {}: {}",
                    entry.watchId.value(),
                    destination.error().toString()
                );
                return;
            }
            queueUpload(Upload{
                .watchId = entry.watchId,
                .change = change,
                .destination = *destination,
                .checkServer = checkServer,
            });
        }

        void onDeleted(Entry& entry, Paths::LocalPath const& path)
        {
            if (entry.downloadedCopy() && path == entry.localPath)
                return awaitCopyReturn(entry);
            if (!entry.options.deleteRemoteOnLocalDelete)
                return;
            // Deleted and created anew before the deletion is handled, the way git rewrites files: the upload of the
            // new one replaces the server copy, which a delete before it only makes look changed.
            if (isThereAgain(entry, path))
            {
                Log::debug("FileTracking: '{}' is there again, not deleting it on the server.", path.toUtf8());
                return;
            }
            if (entry.pendingUploads.removeRemoteWhenDone(path))
            {
                // A deletion reported after the path came back (Windows holds deletions back) stops nothing.
                if (isGoneLocally(path))
                    stopUpload(entry, path, PartialFile::Discarded);
                return;
            }
            if (!entry.serverContent.has(path))
            {
                // The upload of a directory above may have sent it before it went, it is looked at once that ended.
                if (entry.pendingUploads.covers(path))
                {
                    entry.pendingUploads.start({.path = path});
                    return progressChanged(entry);
                }
                Log::debug("FileTracking: '{}' is not on the server, nothing to delete there.", path.toUtf8());
                return;
            }
            // Never recursive: a directory deleted entry by entry reports its content first, so only one moved away
            // as a whole (to the trash) still has known content, and its server copy keeps it. Until its content
            // turns up elsewhere in the item or is deleted after all, see removeMovedAway and removeEmptiedKeptAbove.
            if (entry.serverContent.hasBelow(path))
            {
                entry.serverContent.keep(path);
                if (path == entry.localPath && wasDeletedWithItsContent(entry))
                {
                    Log::info(
                        "FileTracking: '{}' was deleted with its content, deleting its server tree too.", path.toUtf8()
                    );
                    return deleteLostTree(entry, path);
                }
                Log::info(
                    "FileTracking: not deleting the server's copy of directory '{}', it was removed with its content.",
                    path.toUtf8()
                );
                return;
            }
            const auto remotePath = remotePathOf(entry, path);
            if (!remotePath)
            {
                Log::error(
                    "FileTracking: not mirroring the deletion of '{}': {}", path.toUtf8(), remotePath.error().toString()
                );
                return;
            }
            removeRemote(entry, path, *remotePath);
        }

        /**
         * @brief Whether the watched directory of @p entry, gone from its path, was deleted rather than moved away.
         *        FSEvents loses the deletions below a watched directory removed with its content, its identity on
         *        its volume tells instead, see isDeletedFromItsVolume.
         */
        bool wasDeletedWithItsContent(Entry const& entry) const
        {
            auto const* const watch = localWatches->find(entry.watchId);
            if (!watch || !watch->place().self)
                return false;
            return isDeletedFromItsVolume(*watch->place().self, watch->directory()).value_or(false);
        }

        /**
         * @brief The watch lost the changes in @p directory, what is there now is compared with what the server is
         *        known to have. A directory around the item, the instance directory of a downloaded copy, stands for
         *        the item.
         */
        void onMissed(Entry& entry, Paths::LocalPath const& directory)
        {
            const auto scope = entry.localPath.isWithin(directory) ? entry.localPath : directory;
            if (!scope.isWithin(entry.localPath))
                return;
            const auto status = localStatusOf(scope);
            if (!status)
            {
                Log::error(
                    "FileTracking: watch {} lost the changes in '{}', which cannot be checked: {}",
                    entry.watchId.value(),
                    scope.toUtf8(),
                    Utility::errorMessage(status.error())
                );
                return;
            }
            Log::info(
                "FileTracking: watch {} lost the changes in '{}', looking at it anew", entry.watchId.value(), scope.toUtf8()
            );
            if (!*status)
            {
                if (entry.downloadedCopy())
                    return onDeleted(entry, scope);
                if (goneWatchedDirectoryWasDeleted)
                    return deleteLostTree(entry, scope);
                // Like any directory that left with its content. The item's own removal comes from its watch.
                if (scope != entry.localPath)
                    onDeleted(entry, scope);
                return;
            }
            const auto watchId = entry.watchId;
            deleteGoneBelow(entry, scope);
            auto* stillWatched = find(watchId);
            if (!stillWatched)
                return;
            const bool isDirectory = std::filesystem::is_directory(**status);
            if (!isDirectory && isOnTheServerAsItIs(*stillWatched, scope))
                return;
            onContentChanged(*stillWatched, {.path = scope, .isDirectory = isDirectory});
        }

        /**
         * @brief @p directory was deleted with its tree while its watch lost the changes, the deletions of the last
         *        entries among them: what is known there is deleted like the watch would have reported it, deepest
         *        first. The item itself waits for its removal, which its watch holds back (see LocalItemWatch), or
         *        goes with its last entry when it was kept.
         */
        void deleteLostTree(Entry const& entry, Paths::LocalPath const& directory)
        {
            const auto watchId = entry.watchId;
            for (auto const& known : entry.serverContent.deepestFirst(directory))
            {
                auto* stillWatched = find(watchId);
                if (!stillWatched)
                    return;
                // Removing the last entry of a kept directory removes the directory too.
                if (known == stillWatched->localPath || !stillWatched->serverContent.has(known))
                    continue;
                onDeleted(*stillWatched, known);
            }
        }

        /**
         * @brief Deletes what is known below @p directory and gone locally like the watch would have reported it, a
         *        directory as a whole: one that left with its content keeps its server copy.
         */
        void deleteGoneBelow(Entry const& entry, Paths::LocalPath const& directory)
        {
            const auto deepestFirst = entry.serverContent.deepestFirst(directory);
            std::vector<Paths::LocalPath> gone;
            for (auto const& known : deepestFirst | std::views::reverse)
            {
                const auto belowGone = std::ranges::any_of(
                    gone,
                    [&known](Paths::LocalPath const& removed)
                    {
                        return known.isWithin(removed);
                    }
                );
                if (known != directory && !belowGone && isGoneLocally(known))
                    gone.push_back(known);
            }
            const auto watchId = entry.watchId;
            for (auto const& removed : gone)
                if (auto* stillWatched = find(watchId))
                    onDeleted(*stillWatched, removed);
        }

        /**
         * @brief Whether the server's copy of the file at @p path holds what the file holds now: an upload sent it in
         *        the state it has, or, for what a download brought, it has the modification time and size.
         */
        static bool isOnTheServerAsItIs(Entry const& entry, Paths::LocalPath const& path)
        {
            if (const auto sent = entry.serverContent.sentFrom(path))
                return sent == sourceStateOf(path.native());
            const auto states = itemStatesOf(path.native());
            return states.size() == 1u &&
                entry.serverContent.fileState(path) ==
                ServerState{.mtime = states.front().mtime, .size = states.front().size};
        }

        /**
         * @brief Whether @p path, known on the server, is there again locally as the same kind, a directory or not.
         *        A path that cannot be checked is not, removeRemote() keeps its server copy.
         */
        bool isThereAgain(Entry const& entry, Paths::LocalPath const& path) const
        {
            const auto status = localStatusOf(path);
            if (!status || !*status)
                return false;
            if (entry.serverContent.hasDirectory(path))
                return std::filesystem::is_directory(**status);
            return entry.serverContent.fileState(path) && !std::filesystem::is_directory(**status);
        }

        /**
         * @brief The downloaded copy of @p entry was reported deleted or renamed away: its watch ends unless the copy
         *        is back within copyReturnTime.
         */
        void awaitCopyReturn(Entry& entry)
        {
            auto* copy = entry.downloadedCopy();
            if (!copy || copy->returnWait)
                return;
            Log::debug("FileTracking: the copy of watch {} went away, waiting for it", entry.watchId.value());
            copy->returnWait = std::make_unique<boost::asio::steady_timer>(strand, copyReturnTime);
            copy->returnWait->async_wait(
                [weak = weak_from_this(), watchId = entry.watchId](boost::system::error_code const& error)
                {
                    if (error)
                        return;
                    if (const auto self = weak.lock())
                        self->endUnlessCopyReturned(watchId);
                }
            );
        }

        void endUnlessCopyReturned(Ids::WatchId const& watchId)
        {
            auto* entry = find(watchId);
            if (!entry || !entry->downloadedCopy())
                return;
            entry->downloadedCopy()->returnWait.reset();
            if (!isGoneLocally(entry->localPath))
                return;
            Log::info(
                "FileTracking: the copy {} of watch {} was deleted or renamed, its watch ends",
                entry->localPath.toUtf8(),
                watchId.value()
            );
            auto retired = retire(watchId, watchStopOf(SharedData::WatchStopReason::CopyGone));
            if (!retired)
                return;
            view->showEnded({.reason = SharedData::WatchEndReason::CopyGone, .entries = {*std::move(retired)}});
            showOrphanedCopies();
        }

        /**
         * @brief The watched item itself moved to @p newPath: the entry follows it, the server copy moves along when
         *        the entry mirrors moves.
         */
        void onItemMoved(Entry& entry, Paths::LocalPath const& newPath)
        {
            const bool mirrorsMoves = entry.options.moveRemoteOnLocalMove;
            const auto newRemotePath = mirrorsMoves ? renamedRemotePath(entry.remotePath, newPath)
                                                    : std::expected<Paths::RemotePath, std::string>{entry.remotePath};
            if (!newRemotePath)
            {
                Log::warn(
                    "FileTracking: not following the move of watch {}: {}", entry.watchId.value(), newRemotePath.error()
                );
                return;
            }
            // The server copy may not follow onto the path of another watch, which writes there.
            if (const auto overlap = remoteOverlapOf(*newRemotePath, entry.watchId))
                return stopOverlapping(entry.watchId, newPath, *newRemotePath, *overlap);
            Log::info(
                "FileTracking: watch {} moved from '{}' to '{}'",
                entry.watchId.value(),
                entry.localPath.toUtf8(),
                newPath.toUtf8()
            );
            const auto oldPath = entry.localPath;
            // An upload of the item that is on its way writes the old name: it stops and goes on under the new one,
            // or its server copy follows once it ended.
            if (mirrorsMoves)
                stopUpload(entry, oldPath, PartialFile::Kept);
            const bool deferred = entry.pendingUploads.moveWhenDone(oldPath, newPath);
            // Without mirrored moves the server copy keeps its name, the new local name stands for it.
            if (!mirrorsMoves)
                entry.serverContent.move(oldPath, newPath);
            else if (!deferred && entry.serverContent.has(oldPath))
                renameRemote(entry, oldPath, newPath, entry.remotePath, *newRemotePath);
            entry.pendingUploads.moveBelow(oldPath, newPath, /*serverCopiesFollow=*/true);
            entry.localPath = newPath;
            entry.remotePath = *newRemotePath;
            show(entry);
        }

        /**
         * @brief The item of the watch @p watchId moved to @p movedTo, its server copy would follow to @p wouldGoTo,
         *        which @p overlap holds or lies below: the watch stops, its server copy stays where it is.
         */
        void stopOverlapping(
            Ids::WatchId watchId,
            Paths::LocalPath const& movedTo,
            Paths::RemotePath const& wouldGoTo,
            RemoteOverlap const& overlap
        )
        {
            Log::warn(
                "FileTracking: stopping watch {}, its item moved to '{}' and its server copy would go to '{}', which "
                "overlaps '{}' of another watch",
                watchId.value(),
                movedTo.toUtf8(),
                wouldGoTo.value(),
                overlap.remotePath.value()
            );
            auto stop = watchStopOf(SharedData::WatchStopReason::Overlap);
            stop.otherWatchId = overlap.watchId;
            stop.otherWatchName = overlap.watchName;
            stop.movedTo = movedTo;
            stop.wouldGoTo = wouldGoTo;
            auto retired = retire(watchId, std::move(stop));
            if (!retired)
                return;
            view->showEnded({.reason = SharedData::WatchEndReason::Overlap, .entries = {*std::move(retired)}});
            showOrphanedCopies();
        }

        void onMoved(Entry& entry, Paths::LocalPath const& oldPath, LocalChange const& change)
        {
            const auto oldRemotePath = remotePathOf(entry, oldPath);
            const auto newRemotePath = remotePathOf(entry, change.path);
            if (!oldRemotePath || !newRemotePath)
            {
                Log::warn(
                    "FileTracking: not mirroring the move in watch {}: {}",
                    entry.watchId.value(),
                    (oldRemotePath ? newRemotePath.error() : oldRemotePath.error()).toString()
                );
                return;
            }

            if (meaningOfMoveIn(entry, oldPath, change.path) == MoveMeaning::Rename)
            {
                // An upload of the source on its way writes the old name: a file stops and goes on under the new one,
                // or the server copy follows once the upload ended.
                stopUpload(entry, oldPath, PartialFile::Kept);
                if (entry.pendingUploads.moveWhenDone(oldPath, change.path))
                    return;
                const bool renamed = renameRemote(entry, oldPath, change.path, *oldRemotePath, *newRemotePath);
                entry.pendingUploads.moveBelow(oldPath, change.path, /*serverCopiesFollow=*/renamed);
                return;
            }

            Log::info("FileTracking: '{}' replaced '{}', uploading it.", oldPath.toUtf8(), change.path.toUtf8());
            if (entry.options.moveRemoteOnLocalMove)
            {
                if (entry.pendingUploads.removeRemoteWhenDone(oldPath))
                    stopUpload(entry, oldPath, PartialFile::Discarded);
                else if (entry.serverContent.has(oldPath) && !entry.serverContent.hasBelow(oldPath))
                    removeRemote(entry, oldPath, *oldRemotePath);
            }
            onContentChanged(entry, change);
        }

        /**
         * @brief What the move of @p oldPath to @p newPath in @p entry does on the server. With mirrored moves an
         *        upload on its way counts as the server copy it brings, and a move into a directory whose upload is on
         *        its way replaces what that upload brings there.
         */
        static MoveMeaning
        meaningOfMoveIn(Entry const& entry, Paths::LocalPath const& oldPath, Paths::LocalPath const& newPath)
        {
            auto const& known = entry.serverContent;
            auto const& pending = entry.pendingUploads;
            if (!entry.options.moveRemoteOnLocalMove)
                return meaningOfMove(known.has(newPath), known.has(oldPath));
            return meaningOfMove(
                known.has(newPath) || pending.covers(newPath), known.has(oldPath) || pending.isPending(oldPath)
            );
        }

        /**
         * @brief Stops the upload of the file at @p path that is on its way, once the remote got it: its partial file
         *        stays for the upload under the new name, see finishMovedUpload(), or goes, see @p partial. Its end
         *        follows as usual.
         */
        void stopUpload(Entry& entry, Paths::LocalPath const& path, PartialFile partial)
        {
            const auto destination = entry.pendingUploads.sentFileTo(path);
            if (!destination)
                return;
            Log::info(
                "FileTracking: stopping the upload of '{}', it was {}.",
                path.toUtf8(),
                partial == PartialFile::Kept ? "renamed" : "deleted"
            );
            if (partial == PartialFile::Kept)
                entry.pendingUploads.keepsPartial(path);
            remote->cancelUpload(*destination, partial);
        }

        /**
         * @brief Renames the server copy of @p oldPath, at @p oldRemotePath, to @p newRemotePath for @p newPath, when
         *        the entry mirrors moves.
         * @return Whether the rename goes, in the server order.
         */
        bool renameRemote(
            Entry& entry,
            Paths::LocalPath const& oldPath,
            Paths::LocalPath const& newPath,
            Paths::RemotePath const& oldRemotePath,
            Paths::RemotePath const& newRemotePath
        )
        {
            if (!entry.options.moveRemoteOnLocalMove)
                return false;
            inOrder(
                {oldRemotePath, newRemotePath},
                [this, watchId = entry.watchId, oldRemotePath, newRemotePath, ended = renameEndOf(entry, newPath)](
                    ServerOperationOrder::Ticket ticket
                )
                {
                    const auto queued =
                        remote->rename(oldRemotePath, newRemotePath, endsAndLeavesOrder(ended, ticket));
                    if (queued)
                        return;
                    Log::error(
                        "FileTracking: failed to queue the remote rename for watch {}: {}",
                        watchId.value(),
                        queued.error()
                    );
                    ended(SharedData::OperationCompletionReason::Failed);
                    leaveOrder(ticket);
                }
            );
            entry.serverContent.move(oldPath, newPath);
            return true;
        }

        /**
         * @brief Hears when the rename of a server copy for @p newPath ended, see remoteChangeOf(). One that failed
         *        left the copy where it was: what is at @p newPath now goes there, compared with the server first.
         */
        TrackerRemote::OnEnded renameEndOf(Entry& entry, Paths::LocalPath newPath)
        {
            return [weak = weak_from_this(),
                       watchId = entry.watchId,
                       newPath = std::move(newPath),
                       ended = remoteChangeOf(entry)](SharedData::OperationCompletionReason reason)
            {
                ended(reason);
                if (reason == SharedData::OperationCompletionReason::Completed)
                    return;
                if (const auto self = weak.lock())
                    self->onRenameFailed(watchId, newPath);
            };
        }

        void onRenameFailed(Ids::WatchId const& watchId, Paths::LocalPath const& newPath)
        {
            auto* entry = find(watchId);
            if (!entry)
                return;
            Log::warn("FileTracking: the server's copy was not renamed to '{}', uploading it there", newPath.toUtf8());
            entry->serverContent.forget(newPath);
            const auto status = localStatusOf(newPath);
            if (!status || !*status)
                return;
            onContentChanged(*entry, {.path = newPath, .isDirectory = std::filesystem::is_directory(**status)});
        }

        /**
         * @brief Deletes the server copy of @p local, unless @p local cannot be checked: it may still exist.
         */
        void removeRemote(Entry& entry, Paths::LocalPath const& local, Paths::RemotePath const& remotePath)
        {
            if (const auto status = localStatusOf(local); !status)
            {
                Log::error(
                    "FileTracking: not deleting '{}' for watch {}, '{}' cannot be checked: {}",
                    remotePath.value(),
                    entry.watchId.value(),
                    local.toUtf8(),
                    Utility::errorMessage(status.error())
                );
                return;
            }
            Log::info(
                "FileTracking: deleting '{}' for watch {}, it was deleted locally",
                remotePath.value(),
                entry.watchId.value()
            );
            entry.serverContent.forget(local);
            inOrder(
                {remotePath},
                [this, watchId = entry.watchId, remotePath, ended = remoteChangeOf(entry)](
                    ServerOperationOrder::Ticket ticket
                )
                {
                    const auto queued = remote->remove(remotePath, endsAndLeavesOrder(ended, ticket));
                    if (queued)
                        return;
                    Log::error(
                        "FileTracking: failed to queue the remote delete for watch {}: {}",
                        watchId.value(),
                        queued.error()
                    );
                    ended(SharedData::OperationCompletionReason::Failed);
                    leaveOrder(ticket);
                }
            );
            removeEmptiedKeptAbove(entry, local);
        }

        /**
         * @brief A kept directory above @p removed that nothing is known below anymore goes too once it is no
         *        directory locally anymore: its deletion arrived before that of its content (FSEvents may report
         *        them so).
         */
        void removeEmptiedKeptAbove(Entry& entry, Paths::LocalPath const& removed)
        {
            if (!entry.options.deleteRemoteOnLocalDelete)
                return;
            const auto emptied = entry.serverContent.emptiedKeptAbove(removed);
            if (emptied.empty() || isDirectoryLocally(emptied.front()))
                return;
            // Deleting it looks further up.
            if (const auto remotePath = remotePathOf(entry, emptied.front()))
                removeRemote(entry, emptied.front(), *remotePath);
        }

        /**
         * @brief @p uploaded, a directory the server got as a whole, may be one moved within the item that the watch
         *        reported as a removal and an addition: a kept directory gone locally whose known content it holds
         *        moved there, its server copy goes entry by entry.
         */
        void removeMovedAway(Entry& entry, Paths::LocalPath const& uploaded)
        {
            if (!entry.options.moveRemoteOnLocalMove)
                return;
            for (auto const& kept : entry.serverContent.keptDirectories())
            {
                if (kept.isWithin(uploaded) || uploaded.isWithin(kept) || !isGoneLocally(kept) ||
                    !entry.serverContent.holdsAllBelow(uploaded, kept))
                    continue;
                Log::info(
                    "FileTracking: '{}' moved to '{}', deleting the server's copy at its old place.",
                    kept.toUtf8(),
                    uploaded.toUtf8()
                );
                removeKnownTree(entry, kept);
            }
        }

        /**
         * @brief Whether the server is known to have the other kind at @p path than an upload of a directory
         *        (@p isDirectory) or a file would write, and it may be deleted for it.
         */
        bool hasOtherKindInTheWay(Entry const& entry, Paths::LocalPath const& path, bool isDirectory) const
        {
            if (!entry.options.deleteRemoteOnLocalDelete)
                return false;
            return isDirectory ? entry.serverContent.fileState(path).has_value()
                               : entry.serverContent.hasDirectory(path);
        }

        /**
         * @brief Deletes what is known on the server at and below @p path entry by entry, deepest first, which never
         *        takes anything along the tracker does not know of.
         */
        void removeKnownTree(Entry& entry, Paths::LocalPath const& path)
        {
            for (auto const& known : entry.serverContent.deepestFirst(path))
            {
                // Removing the last entry of a kept directory removes the directory too.
                if (!entry.serverContent.has(known))
                    continue;
                if (const auto remotePath = remotePathOf(entry, known))
                    removeRemote(entry, known, *remotePath);
            }
        }

        /**
         * @brief Whether the server's copy of the file of @p change holds what the file holds now: an upload sent it
         *        in the state it has, the change was reported after that. Not while an upload that takes the file
         *        along is on its way, the change goes after it.
         */
        static bool isSentAsItIs(Entry const& entry, LocalChange const& change)
        {
            if (change.isDirectory || entry.pendingUploads.covers(change.path))
                return false;
            const auto sent = entry.serverContent.sentFrom(change.path);
            return sent && sent == sourceStateOf(change.path.native());
        }

        /**
         * @brief Uploads a change unless the same path, or a directory above it, is already on its way; the change
         *        then goes once that upload ended. It starts in the server order, after what was decided before on
         *        its path. A file the server has as it is stays, a directory the server has goes by what it lacks.
         */
        void queueUpload(Upload upload)
        {
            auto* entry = find(upload.watchId);
            if (!entry)
                return;
            if (isSentAsItIs(*entry, upload.change))
            {
                Log::debug(
                    "FileTracking: the server has '{}' as it is, not uploading it.", upload.change.path.toUtf8()
                );
                return discardPartial(upload.continuesPartialOf);
            }
            if (upload.change.isDirectory && !entry->pendingUploads.covers(upload.change.path) &&
                entry->serverContent.hasDirectory(upload.change.path))
                return uploadWhatTheServerLacks(*entry, upload);
            const bool starts = entry->pendingUploads.start(upload.change);
            progressChanged(*entry);
            if (!starts)
                return discardPartial(upload.continuesPartialOf);
            showUploading(*entry);
            upload.ticket.reset();
            // A continued upload takes the partial file of the old name, after the stopped upload ended.
            std::vector<Paths::RemotePath> paths{upload.destination};
            if (upload.continuesPartialOf)
                paths.push_back(*upload.continuesPartialOf);
            inOrder(
                std::move(paths),
                [this, upload = std::move(upload)](ServerOperationOrder::Ticket ticket) mutable
                {
                    upload.ticket = ticket;
                    checkServerThenUpload(std::move(upload));
                }
            );
        }

        /**
         * @brief @p upload is of a directory the server has: what is below it goes on its own where the server lacks
         *        it, a file no upload sent as it is and a directory with its content. A link the server has stays,
         *        a change of its own retargets it. Nothing is compared with the server, the upload of the whole
         *        directory would not either: the states recorded after the last one may be newer than what it sent.
         */
        void uploadWhatTheServerLacks(Entry& entry, Upload const& upload)
        {
            Log::debug(
                "FileTracking: the server has the directory '{}', uploading what it lacks.", upload.change.path.toUtf8()
            );
            const auto watchId = entry.watchId;
            std::vector<Paths::LocalPath> newDirectories;
            for (auto const& state : itemStatesOf(upload.change.path.native()))
            {
                const auto path = Paths::LocalPath::make(state.path);
                if (!path || *path == upload.change.path || !path->isWithin(upload.change.path))
                    continue;
                auto* stillWatched = find(watchId);
                if (!stillWatched)
                    return;
                const auto inNewDirectory = std::ranges::any_of(
                    newDirectories,
                    [&path](Paths::LocalPath const& directory)
                    {
                        return path->isWithin(directory);
                    }
                );
                const bool known = state.isDirectory ? stillWatched->serverContent.hasDirectory(*path)
                                                     : state.isSymlink && stillWatched->serverContent.fileState(*path);
                if (inNewDirectory || known)
                    continue;
                if (state.isDirectory)
                    newDirectories.push_back(*path);
                if (const auto destination = remotePathOf(*stillWatched, *path))
                    queueUpload(
                        Upload{
                            .watchId = watchId,
                            .change = {.path = *path, .isDirectory = state.isDirectory},
                            .destination = *destination,
                            .checkServer = false,
                        }
                    );
            }
        }

        void checkServerThenUpload(Upload upload)
        {
            auto* entry = find(upload.watchId);
            // A path that moved away goes again at the new one, see enqueueUpload.
            if (entry && !entry->pendingUploads.movedAway(upload.change.path))
            {
                const auto status = localStatusOf(upload.change.path);
                if (!status)
                    return onUncheckable(upload, status.error());
                if (!*status)
                    return onGoneLocally(*entry, upload);
                // Turned into the other kind while the change waited, or the server has the other kind there: it goes
                // again as what it is now, after the server copy in the way (see followUpUpload).
                const bool isDirectory = std::filesystem::is_directory(**status);
                if (isDirectory != upload.change.isDirectory ||
                    hasOtherKindInTheWay(*entry, upload.change.path, isDirectory))
                {
                    entry->pendingUploads.start({.path = upload.change.path, .isDirectory = isDirectory});
                    return finishUpload(upload, UploadOutcome::NotUploaded);
                }
            }
            if (!entry || !entry->options.checkRemoteChanges || !upload.checkServer)
                return enqueueUpload(std::move(upload));
            // The upload of a regular file compares it, right before it replaces the server's copy.
            if (!upload.change.isDirectory && !isSymlink(upload.change.path))
            {
                auto expected = ExpectedDestination{.file = entry->serverContent.fileState(upload.change.path)};
                return enqueueUpload(std::move(upload), std::move(expected));
            }
            if (const auto change = serverChangeAt(*entry, upload))
                return askServerChanged(std::move(upload), *change);
            enqueueUpload(std::move(upload));
        }

        /**
         * @brief How the server's copy at the destination of @p upload, a directory or a symbolic link, is not what
         *        this tracker left there, nothing when it is. A symbolic link is compared as itself.
         */
        std::optional<ServerChange> serverChangeAt(Entry const& entry, Upload const& upload)
        {
            const auto current =
                isSymlink(upload.change.path) ? remote->linkStat(upload.destination) : remote->stat(upload.destination);
            if (!current)
            {
                Log::warn(
                    "FileTracking: cannot check '{}' on the server: {}", upload.destination.value(), current.error()
                );
                // An unanswered check counts as changed.
                return ServerChange{};
            }
            if (upload.change.isDirectory)
            {
                if (!current->has_value() || entry.serverContent.has(upload.change.path))
                    return std::nullopt;
                return serverChangeOf(/*knownThere=*/false, *current);
            }
            const auto known = entry.serverContent.fileState(upload.change.path);
            if (!serverChanged(known, *current))
                return std::nullopt;
            return serverChangeOf(known.has_value(), *current);
        }

        /**
         * @brief The path of @p upload is gone by the time it would go: there is nothing to ask about or to upload,
         *        and the deletion is mirrored, which the watch may not report (macOS can report a removed directory
         *        as modified only). The change counts as nothing done, the deletion counts on its own.
         */
        void onGoneLocally(Entry const& entry, Upload const& upload)
        {
            Log::debug("FileTracking: '{}' is gone, not uploading it.", upload.change.path.toUtf8());
            const auto watchId = entry.watchId;
            finishUpload(upload, UploadOutcome::NotUploaded, /*carried=*/0u);
            if (auto* stillWatched = find(watchId))
                onDeleted(*stillWatched, upload.change.path);
        }

        /**
         * @brief The path of @p upload cannot be checked (@p error), it may still exist: it is neither uploaded nor
         *        deleted on the server.
         */
        void onUncheckable(Upload const& upload, std::error_code const& error)
        {
            Log::error(
                "FileTracking: not uploading '{}', it cannot be checked: {}",
                upload.change.path.toUtf8(),
                Utility::errorMessage(error)
            );
            if (const auto* entry = find(upload.watchId))
                recordFailure(*entry, upload.change.path, upload.destination, failureInfoOf(error));
            finishUpload(upload, UploadOutcome::NotUploaded);
        }

        /**
         * @brief Lists the conflict @p upload raised, the server changed its destination as @p change tells, unless
         *        it no longer stands, see conflictStands(). The upload waits for the answer, see answerConflict().
         */
        void askServerChanged(Upload upload, ServerChange const& change)
        {
            const auto watchId = upload.watchId;
            if (conflictStands(upload))
            {
                auto added = conflicts.add(conflictOf(upload, change), std::move(upload));
                // PendingUploads lets one upload of a path wait at most, this is never expected.
                if (added.replaced)
                {
                    Log::warn(
                        "FileTracking: the conflict about '{}' replaced another one", added.conflict.localPath.toUtf8()
                    );
                    leaveOrder(*added.replaced);
                }
                Log::info(
                    "FileTracking: '{}' changed on the server, waiting for an answer about it",
                    added.conflict.remotePath.value()
                );
                view->showConflict(added.conflict);
            }
            progressChanged(watchId);
        }

        /**
         * @brief Whether the conflict about @p upload still stands, checked when it is raised and once it is answered:
         *        a path that moved away meanwhile goes again at the new one, one that is gone locally has its deletion
         *        mirrored, and nothing is asked about a path that no longer is the watch's. Either ends @p upload.
         */
        bool conflictStands(Upload const& upload)
        {
            const auto* entry = find(upload.watchId);
            if (!entry || entry->pendingUploads.movedAway(upload.change.path) ||
                !upload.change.path.isWithin(entry->localPath))
            {
                finishUpload(upload, UploadOutcome::NotUploaded);
                return false;
            }
            if (isGoneLocally(upload.change.path))
            {
                onGoneLocally(*entry, upload);
                return false;
            }
            return true;
        }

        void answerConflict(Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution)
        {
            auto listed = conflicts.remove(conflictId);
            if (!listed)
            {
                Log::warn("FileTracking: there is no conflict {} to answer", conflictId.value());
                return;
            }
            const auto watchId = listed->conflict.watchId;
            resolveConflict(std::move(listed->context), resolution);
            view->removeConflict(conflictId);
            progressChanged(watchId);
        }

        void answerAllConflicts(Ids::WatchId const& watchId, SharedData::ConflictResolution resolution)
        {
            auto listed = conflicts.removeWatch(watchId);
            if (listed.empty())
            {
                Log::warn("FileTracking: watch {} has no conflicts to answer", watchId.value());
                return;
            }
            Log::info(
                "FileTracking: answering {} conflict(s) of watch {} with {}",
                listed.size(),
                watchId.value(),
                Utility::enumToString(resolution)
            );
            for (auto& conflict : listed)
            {
                resolveConflict(std::move(conflict.context), resolution);
                view->removeConflict(conflict.conflict.conflictId);
            }
            progressChanged(watchId);
        }

        void resolveConflict(Upload upload, SharedData::ConflictResolution resolution)
        {
            switch (resolution)
            {
                case SharedData::ConflictResolution::ReplaceOnServer:
                    Log::info("FileTracking: replacing the server's version of '{}'.", upload.destination.value());
                    return replaceOnServer(std::move(upload));
                case SharedData::ConflictResolution::KeepServers:
                    Log::info("FileTracking: kept the server's version of '{}'.", upload.destination.value());
                    return declineUpload(upload);
            }
        }

        /**
         * @brief Uploads @p upload over whatever the server has, once the path is checked again.
         */
        void replaceOnServer(Upload upload)
        {
            if (!conflictStands(upload))
                return;
            upload.checkServer = false;
            checkServerThenUpload(std::move(upload));
        }

        void retryFailure(Ids::FailureId const& failureId)
        {
            const auto retried = failures.retry(failureId);
            if (!retried)
            {
                Log::warn("FileTracking: there is no failure {} to retry", failureId.value());
                return;
            }
            view->removeFailure(failureId);
            retryUpload(*retried);
        }

        /**
         * @brief Uploads the path of @p failure again like a change of it, compared with the server first, also when
         *        its watch does not upload changes on its own.
         */
        void retryUpload(SharedData::UploadFailure const& failure)
        {
            const auto* entry = find(failure.watchId);
            if (!entry)
                return;
            const auto destination = remotePathOf(*entry, failure.localPath);
            if (!destination)
            {
                Log::warn(
                    "FileTracking: not retrying '{}' in watch {}: {}",
                    failure.localPath.toUtf8(),
                    failure.watchId.value(),
                    destination.error().toString()
                );
                return;
            }
            Log::info("FileTracking: retrying the upload of '{}'", failure.localPath.toUtf8());
            queueUpload(
                Upload{
                    .watchId = failure.watchId,
                    .change = {.path = failure.localPath, .isDirectory = isDirectoryLocally(failure.localPath)},
                    .destination = *destination,
                }
            );
        }

        void dismissFailure(Ids::FailureId const& failureId)
        {
            if (!failures.remove(failureId))
            {
                Log::warn("FileTracking: there is no failure {} to dismiss", failureId.value());
                return;
            }
            view->removeFailure(failureId);
        }

        void retryAllFailures(Ids::WatchId const& watchId)
        {
            const auto listed = failures.failuresOf(watchId);
            if (listed.empty())
                Log::warn("FileTracking: watch {} has no failures to retry", watchId.value());
            for (auto const& failure : listed)
                retryFailure(failure.failureId);
        }

        void dismissAllFailures(Ids::WatchId const& watchId)
        {
            const auto listed = failures.failuresOf(watchId);
            if (listed.empty())
                Log::warn("FileTracking: watch {} has no failures to dismiss", watchId.value());
            for (auto const& failure : listed)
                dismissFailure(failure.failureId);
        }

        /**
         * @brief Lists that the upload of @p localPath to @p remotePath failed as @p failure, or counts one attempt
         *        more of it. Not for a path that is gone or moved away, its deletion or move follows.
         * @return Whether it was listed.
         */
        bool recordFailure(
            Entry const& entry,
            Paths::LocalPath const& localPath,
            Paths::RemotePath const& remotePath,
            UploadFailureInfo const& failure
        )
        {
            if (entry.pendingUploads.movedAway(localPath) || isGoneLocally(localPath))
                return false;
            const auto listed = failures.record({
                .failureId = {},
                .watchId = entry.watchId,
                .localPath = localPath,
                .remotePath = remotePath,
                .reason = failure.reason,
                .detail = failure.detail,
                .failedAt = secondsSinceEpochNow(),
                .attempts = 1u,
            });
            Log::warn(
                "FileTracking: uploading '{}' failed ({}, attempt {}): {}",
                localPath.toUtf8(),
                Utility::enumToString(listed.reason),
                listed.attempts,
                listed.detail.value_or("")
            );
            view->showFailure(listed);
            return true;
        }

        /**
         * @brief @p localPath of the watch @p watchId uploaded, its failure is over.
         */
        void clearFailure(Ids::WatchId const& watchId, Paths::LocalPath const& localPath)
        {
            if (const auto cleared = failures.removePath(watchId, localPath))
                view->removeFailure(cleared->failureId);
        }

        /**
         * @brief The directory upload @p upload completed but left out @p failedEntries: they are listed, the failures
         *        at and below the directory that it did not leave out are over.
         */
        void
        recordLeftOut(Entry const& entry, Upload const& upload, std::vector<FailedUploadEntry> const& failedEntries)
        {
            std::set<Paths::LocalPath> leftOut;
            for (auto const& failed : failedEntries)
            {
                const auto remotePath = remotePathOfChange(failed.path, upload.change.path, upload.destination);
                if (remotePath && recordFailure(entry, failed.path, *remotePath, failed.failure))
                    leftOut.insert(failed.path);
            }
            for (auto const& cleared : failures.removeWithin(entry.watchId, upload.change.path, leftOut))
                view->removeFailure(cleared.failureId);
        }

        void removeStopped(Ids::WatchId const& watchId)
        {
            if (stopped.erase(watchId) == 0)
            {
                Log::warn("FileTracking: there is no stopped watch {} to remove", watchId.value());
                return;
            }
            Log::info("FileTracking: removing the stopped watch {}", watchId.value());
            view->removeEntry(watchId);
            // A stopped copy is an orphan from now on.
            showOrphanedCopies();
        }

        void removeAllStopped()
        {
            const auto removed = std::exchange(stopped, {});
            Log::info("FileTracking: removing {} stopped watch(es)", removed.size());
            for (auto const& watchId : std::views::keys(removed))
                view->removeEntry(watchId);
            showOrphanedCopies();
        }

        void listOrphanedCopies()
        {
            showOrphanedCopies();
        }

        std::expected<void, std::string> removeOrphanedCopy(Ids::InstanceId const& instanceId)
        {
            const auto removed = refuseStoppedCopy(instanceId).and_then(
                [this, &instanceId]()
                {
                    return instances->removeOrphanedInstance(instanceId);
                }
            );
            if (!removed)
                Log::warn("FileTracking: {}", removed.error());
            showOrphanedCopies();
            return removed;
        }

        void removeAllOrphanedCopies()
        {
            const auto listed = orphanedCopies();
            Log::info("FileTracking: removing {} orphaned copies", listed.copies.size());
            for (auto const& copy : listed.copies)
                if (const auto removed = instances->removeOrphanedInstance(copy.instanceId); !removed)
                    Log::warn("FileTracking: {}", removed.error());
            showOrphanedCopies();
        }

        /**
         * @brief The orphaned copies as the view lists them: those of stopped watches are left out, the view shows
         *        them as stopped until they are removed.
         */
        SharedData::OrphanedCopies orphanedCopies() const
        {
            auto copies = instances->listOrphaned();
            std::erase_if(
                copies,
                [this](SharedData::OrphanedCopy const& copy)
                {
                    return isStoppedCopy(copy.instanceId);
                }
            );
            return {.copies = std::move(copies)};
        }

        void showOrphanedCopies()
        {
            view->showOrphanedCopies(orphanedCopies());
        }

        bool isStoppedCopy(Ids::InstanceId const& instanceId) const
        {
            return std::ranges::any_of(
                std::views::values(stopped),
                [&instanceId](SharedData::TrackedEntry const& entry)
                {
                    return entry.instanceId == instanceId;
                }
            );
        }

        std::expected<void, std::string> refuseStoppedCopy(Ids::InstanceId const& instanceId) const
        {
            if (!isStoppedCopy(instanceId))
                return {};
            return std::unexpected{fmt::format(
                "Cannot remove the orphaned instance '{}': it is the copy of a stopped watch", instanceId.value()
            )};
        }

        /**
         * @brief The change is not uploaded, and the changes that arrived while its conflict waited wait for the next
         *        save, which raises it again.
         */
        void declineUpload(Upload const& upload)
        {
            if (auto* entry = find(upload.watchId))
                entry->pendingUploads.changesHandled(upload.change.path);
            finishUpload(upload, UploadOutcome::Declined);
        }

        /**
         * @param expected What the server must still have at the destination, see UploadRequest::expectedDestination.
         */
        void enqueueUpload(Upload upload, std::optional<ExpectedDestination> expected = std::nullopt)
        {
            auto* entry = find(upload.watchId);
            // A path that moved meanwhile cannot be read, the upload goes again at the new one once this one ended.
            if (!entry || entry->pendingUploads.movedAway(upload.change.path))
                return finishUpload(upload, UploadOutcome::NotUploaded);
            // The upload reads the file when it runs, the changes until now are in it.
            entry->pendingUploads.changesHandled(upload.change.path);
            if (upload.change.isDirectory)
                entry->pendingUploads.carries(upload.change.path, entryCountOf(upload.change.path.native()));
            entry->pendingUploads.sent(upload.change.path, upload.destination, upload.change.isDirectory);
            entry->progress.uploadQueued(upload.change.path, fileSizeOf(upload.change.path));
            progressChanged(*entry);
            const auto queued = remote->upload(
                UploadRequest{
                    .source = upload.change.path,
                    .destination = upload.destination,
                    .refreshListing = upload.refreshListing,
                    // Below the item a directory renamed without mirroring has no server counterpart yet.
                    .createMissingDirectories = upload.change.path != entry->localPath,
                    .expectedDestination = expected,
                    .continuesPartialOf = upload.continuesPartialOf,
                },
                [weak = weak_from_this(), upload, expected](UploadEnd const& ended)
                {
                    if (const auto self = weak.lock())
                        self->onUploadEnded(upload, expected, ended);
                },
                [weak = weak_from_this(),
                    watchId = upload.watchId,
                    path = upload.change.path](UploadProgress const& progress)
                {
                    if (const auto self = weak.lock())
                        self->onUploadProgress(watchId, path, progress);
                }
            );
            if (queued)
                return;
            // A temporary file an editor already renamed or removed ends up here.
            Log::warn("FileTracking: did not upload '{}': {}", upload.change.path.toUtf8(), queued.error());
            if (auto* stillWatched = find(upload.watchId))
            {
                stillWatched->progress.uploadEnded(upload.change.path, WatchProgressMeter::UploadResult::NotCompleted);
                recordFailure(
                    *stillWatched,
                    upload.change.path,
                    upload.destination,
                    {.reason = SharedData::UploadFailureReason::NotQueued, .detail = queued.error()}
                );
            }
            finishUpload(upload, UploadOutcome::NotUploaded);
        }

        /**
         * @brief Once an upload ended, what the server has now is the state the next check compares with. The upload
         *        stays on its way until the remote told that state.
         * @param expected What the upload expected the server to have at its destination.
         */
        void
        onUploadEnded(Upload const& upload, std::optional<ExpectedDestination> const& expected, UploadEnd const& ended)
        {
            auto* entry = find(upload.watchId);
            if (entry)
                entry->progress.uploadEnded(upload.change.path, uploadResultOf(ended));
            // The server's copy changed meanwhile and stayed, the upload asks about it now unless the user stopped
            // checking since.
            if (entry && ended.changedOnServer)
            {
                if (entry->options.checkRemoteChanges)
                    return askServerChanged(
                        upload, serverChangeOf(expected && expected->file.has_value(), ended.destinationState)
                    );
                return enqueueUpload(upload);
            }
            if (ended.reason != SharedData::OperationCompletionReason::Completed || !entry)
            {
                // Nobody continues the partial file of a watch that ended meanwhile.
                if (!entry && !upload.change.isDirectory)
                    discardPartial(upload.destination);
                if (entry && ended.reason == SharedData::OperationCompletionReason::Failed)
                    recordFailure(
                        *entry, upload.change.path, upload.destination, ended.failure.value_or(UploadFailureInfo{})
                    );
                return finishUpload(upload, UploadOutcome::NotUploaded);
            }
            // Uploads carry the local modification time, so the local states are what the server has now. A link
            // to a directory reports itself as one, the walk of its states would follow it.
            std::optional<std::size_t> carried{};
            if (upload.change.isDirectory && !isSymlink(upload.change.path))
            {
                carried = rememberUploadedStates(*entry, upload.change.path, ended.failedEntries);
                rememberSentButGone(*entry, upload, ended.sentFiles);
                removeMovedAway(*entry, upload.change.path);
                recordLeftOut(*entry, upload, ended.failedEntries);
            }
            else
            {
                if (ended.destinationState)
                    entry->serverContent.rememberFile(upload.change.path, *ended.destinationState);
                clearFailure(entry->watchId, upload.change.path);
            }
            for (auto const& sent : ended.sentFiles)
                if (sent.state && sent.path.isWithin(upload.change.path))
                    entry->serverContent.rememberSent(sent.path, *sent.state);
            finishUpload(upload, UploadOutcome::Uploaded, carried);
        }

        /**
         * @brief What the directory upload @p upload wrote that is gone locally by now, which rememberStates() cannot
         *        see: the files of @p sent with the directories above them, and the directory itself when the server
         *        has it. Their deletions follow as reported, see onDeleted.
         */
        void rememberSentButGone(Entry& entry, Upload const& upload, std::vector<SentFile> const& sent)
        {
            auto const& directory = upload.change.path;
            for (auto const& file : sent)
            {
                if (!file.path.isWithin(directory) || entry.serverContent.fileState(file.path) ||
                    !isGoneLocally(file.path))
                    continue;
                const auto state = serverStateOfSent(entry, file);
                if (!state)
                    continue;
                entry.serverContent.rememberFile(file.path, *state);
                for (auto parent = file.path.parent(); parent && parent->isWithin(directory); parent = parent->parent())
                    entry.serverContent.rememberDirectory(*parent);
            }
            if (entry.serverContent.has(directory) || !isGoneLocally(directory))
                return;
            if (const auto state = remote->linkStat(upload.destination); state && *state)
                entry.serverContent.rememberDirectory(directory);
        }

        /**
         * @brief What the server has of @p file, which an upload wrote: the state it was sent in, or else what the
         *        server tells. Nothing when it has no file there or cannot be asked.
         */
        std::optional<ServerState> serverStateOfSent(Entry const& entry, SentFile const& file)
        {
            if (file.state)
                return serverStateOf(*file.state);
            const auto remotePath = remotePathOf(entry, file.path);
            if (!remotePath)
                return std::nullopt;
            return remote->linkStat(*remotePath).value_or(std::nullopt);
        }

        /**
         * @brief The upload of @p upload is no longer on its way, see followUpUpload. The operations waiting for it
         *        in the server order start then.
         * @param carried The entries it left on the server, it counts as done that many: what a completed directory
         *        upload recorded, the changes below it that arrived meanwhile count when they go. Unset, it counts as
         *        the changes it stood for.
         */
        void
        finishUpload(Upload const& upload, UploadOutcome outcome, std::optional<std::size_t> carried = std::nullopt)
        {
            if (auto* entry = find(upload.watchId))
                followUpUpload(*entry, upload, outcome, carried);
            leaveOrder(upload);
        }

        /**
         * @brief What changed while @p upload was on its way goes now: the path itself once more, the changes below an
         *        uploaded directory, or the removal of a path that went away.
         */
        void
        followUpUpload(Entry& entry, Upload const& upload, UploadOutcome outcome, std::optional<std::size_t> carried)
        {
            auto finished = entry.pendingUploads.finish(upload.change.path);
            showUploading(entry);
            changeEnded(entry, carried.value_or(finished.counted));
            // A directory above that moved took the server copy along.
            const auto serverPath = finished.serverCopyAt.value_or(upload.change.path);
            const auto serverDestination = finished.serverCopyAt
                ? remotePathOf(entry, serverPath).value_or(upload.destination)
                : upload.destination;
            const auto keptPartial = keptPartialOf(upload, outcome, finished);
            if (finished.removeRemote)
            {
                // Deleted while it was on its way but there again by now, the way git flips a path: it goes once
                // more instead and the server copy stays. So does one whose stopped upload left nothing.
                const bool backUnsent = outcome != UploadOutcome::Uploaded &&
                    !isGoneLocally(finished.movedTo.value_or(upload.change.path));
                if (!isThereAgain(entry, serverPath) && !backUnsent)
                {
                    discardPartial(keptPartial);
                    // Everything below a directory went along.
                    if (entry.serverContent.has(serverPath))
                        removeKnownTree(entry, serverPath);
                    else if (outcome == UploadOutcome::Uploaded)
                        removeRemote(entry, serverPath, serverDestination);
                    return;
                }
                finished.changedAgain = true;
            }
            if (finished.movedTo)
                return finishMovedUpload(entry, upload, outcome, finished, serverPath, serverDestination, keptPartial);
            if (finished.changedAgain)
            {
                // Back as the other kind, a directory where a file was or the other way round: the server's copy is
                // in the way and goes first.
                if (hasOtherKindInTheWay(entry, serverPath, finished.isDirectory))
                    removeKnownTree(entry, serverPath);
                auto again = upload;
                again.change.isDirectory = finished.isDirectory;
                again.continuesPartialOf.reset();
                queueUpload(std::move(again));
            }
            discardPartial(keptPartial);
            for (auto const& change : finished.changedBelow)
                onContentChanged(entry, change, /*checkServer=*/false);
        }

        /**
         * @brief The partial file on the server that the path of @p upload left and no upload continues yet: the one
         *        a stopped upload kept, or the one @p upload was to continue but never got to. Nothing once it was
         *        uploaded.
         */
        static std::optional<Paths::RemotePath>
        keptPartialOf(Upload const& upload, UploadOutcome outcome, PendingUploads::Finished const& finished)
        {
            if (outcome == UploadOutcome::Uploaded || upload.change.isDirectory)
                return std::nullopt;
            if (finished.sentTo)
                return finished.partialKept ? finished.sentTo : std::nullopt;
            return upload.continuesPartialOf;
        }

        /**
         * @brief The partial file a stopped upload to @p destination kept goes, no upload continues it. It goes in the
         *        server order, so it never meets the partial file of a later upload of that path.
         */
        void discardPartial(std::optional<Paths::RemotePath> const& destination)
        {
            if (!destination)
                return;
            inOrder(
                {*destination},
                [this, destination = *destination](ServerOperationOrder::Ticket ticket)
                {
                    const auto queued = remote->discardPartialUpload(destination, leavesOrderWhenEnded(ticket));
                    if (queued)
                        return;
                    Log::warn(
                        "FileTracking: cannot remove the partial upload of '{}': {}",
                        destination.value(),
                        queued.error()
                    );
                    leaveOrder(ticket);
                }
            );
        }

        /**
         * @brief The path of @p upload moved while it was on its way (see PendingUploads::moveWhenDone and moveBelow):
         *        its server copy at @p serverPath follows now, and what the upload could not take or what changed
         *        meanwhile goes to the new path.
         */
        void finishMovedUpload(
            Entry& entry,
            Upload const& upload,
            UploadOutcome outcome,
            PendingUploads::Finished const& finished,
            Paths::LocalPath const& serverPath,
            Paths::RemotePath const& serverDestination,
            std::optional<Paths::RemotePath> const& keptPartial
        )
        {
            auto const& movedTo = *finished.movedTo;
            const auto destination = remotePathOf(entry, movedTo);
            if (!destination)
            {
                Log::warn(
                    "FileTracking: not following the move of '{}' in watch {}: {}",
                    upload.change.path.toUtf8(),
                    entry.watchId.value(),
                    destination.error().toString()
                );
                return discardPartial(keptPartial);
            }
            // The server copy moved along with a directory above, or the server renames it now.
            const bool inPlace = serverPath == movedTo;
            const bool renamed = inPlace ||
                ((outcome == UploadOutcome::Uploaded || entry.serverContent.has(serverPath)) &&
                 renameRemote(entry, serverPath, movedTo, serverDestination, *destination));
            const LocalChange moved{.path = movedTo, .isDirectory = upload.change.isDirectory};
            // The upload read the old path, which may have been gone by then, or it was stopped and goes on under the
            // new name. A declined one waits for the next change.
            const bool goesAgain = outcome == UploadOutcome::NotUploaded || finished.changedAgain;
            // A file changed since it was stopped goes anew: the resume compares only the end of the part sent.
            const auto continued = goesAgain && !finished.changedAgain ? keptPartial : std::nullopt;
            if (!continued)
                discardPartial(keptPartial);
            if (goesAgain)
                return queueUpload(Upload{
                    .watchId = entry.watchId,
                    .change = moved,
                    .destination = *destination,
                    .checkServer =
                        outcome == UploadOutcome::Declined || (!renamed && outcome != UploadOutcome::Uploaded),
                    .continuesPartialOf = continued,
                });
            // The states taken after the upload are those of the old path, the local ones are the server's now.
            if (renamed && outcome == UploadOutcome::Uploaded && (moved.isDirectory || inPlace))
            {
                entry.serverContent.forget(upload.change.path);
                rememberStates(entry, movedTo);
            }
            for (auto const& change : finished.changedBelow)
                onContentChanged(entry, change, /*checkServer=*/false);
        }
    };

    Tracker::Tracker(
        boost::asio::strand<boost::asio::any_io_executor> strand,
        TempDirInstanceManager& instances,
        LocalWatchManager& localWatches,
        std::unique_ptr<TrackerRemote> remote,
        std::unique_ptr<TrackerView> view,
        std::string downloadTempSuffix,
        std::function<SharedData::WatchOptions()> defaultOptions
    )
        : impl_{std::make_shared<Implementation>(
              std::move(strand),
              instances,
              localWatches,
              std::move(remote),
              std::move(view),
              std::move(downloadTempSuffix),
              std::move(defaultOptions)
          )}
    {}

    Tracker::~Tracker() = default;

    void Tracker::open(
        Paths::RemotePath const& remotePath,
        std::uint64_t sizeBytes,
        bool watch,
        std::function<void(std::expected<OpenedCopy, OpenFailure> const&)> onOpened
    )
    {
        impl_->open(remotePath, sizeBytes, watch, std::move(onOpened));
    }

    std::expected<Tracker::UploadTarget, std::string>
    Tracker::checkUploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& directory) const
    {
        return impl_->checkUploadAndWatch(item, directory);
    }

    std::expected<void, std::string>
    Tracker::uploadAndWatch(Paths::LocalPath const& item, Paths::RemotePath const& destination)
    {
        return impl_->uploadAndWatch(item, destination);
    }

    void Tracker::stop(Ids::WatchId const& watchId)
    {
        impl_->stop(watchId);
    }

    void Tracker::setOption(Ids::WatchId const& watchId, SharedData::WatchOption option, bool value)
    {
        impl_->setOption(watchId, option, value);
    }

    std::expected<DiscardOutcome, std::string>
    Tracker::discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& path)
    {
        return impl_->discard(instanceId, path);
    }

    void Tracker::answerConflict(Ids::ConflictId const& conflictId, SharedData::ConflictResolution resolution)
    {
        impl_->answerConflict(conflictId, resolution);
    }

    void Tracker::answerAllConflicts(Ids::WatchId const& watchId, SharedData::ConflictResolution resolution)
    {
        impl_->answerAllConflicts(watchId, resolution);
    }

    void Tracker::retryFailure(Ids::FailureId const& failureId)
    {
        impl_->retryFailure(failureId);
    }

    void Tracker::dismissFailure(Ids::FailureId const& failureId)
    {
        impl_->dismissFailure(failureId);
    }

    void Tracker::retryAllFailures(Ids::WatchId const& watchId)
    {
        impl_->retryAllFailures(watchId);
    }

    void Tracker::dismissAllFailures(Ids::WatchId const& watchId)
    {
        impl_->dismissAllFailures(watchId);
    }

    void Tracker::removeStopped(Ids::WatchId const& watchId)
    {
        impl_->removeStopped(watchId);
    }

    void Tracker::removeAllStopped()
    {
        impl_->removeAllStopped();
    }

    void Tracker::listOrphanedCopies()
    {
        impl_->listOrphanedCopies();
    }

    std::expected<void, std::string> Tracker::removeOrphanedCopy(Ids::InstanceId const& instanceId)
    {
        return impl_->removeOrphanedCopy(instanceId);
    }

    void Tracker::removeAllOrphanedCopies()
    {
        impl_->removeAllOrphanedCopies();
    }

    void Tracker::stopAll()
    {
        impl_->stopAll();
    }

    void Tracker::connectionLost()
    {
        impl_->connectionLost();
    }
}
