#include <backend/file_tracking/local_item_watch.hpp>
#include <backend/file_tracking/change_filter.hpp>
#include <backend/file_tracking/change_reporter.hpp>
#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/event_spelling.hpp>
#include <backend/file_tracking/settled_changes.hpp>
#include <utility/localized_message.hpp>
#include <paths/native.hpp>
#include <persistence/state/sftp_options.hpp>
#include <log/log.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FileTracking
{
    struct LocalItemWatch::State : public std::enable_shared_from_this<State>
    {
        /**
         * @brief A rename of the item, held back until it is clear the item does not come back.
         */
        struct PendingRename
        {
            DirectoryChange change;
            /** @brief The changes at the new name meanwhile, reported once the rename is. */
            std::vector<DirectoryChange> changesAtNewName{};
        };

        /**
         * @brief Where the watch followed the item from: the item appearing there again before @p until takes the
         *        watch back, see renameReturnTime.
         */
        struct FollowedRename
        {
            std::filesystem::path from;
            std::chrono::steady_clock::time_point until;
        };

        /**
         * @brief The names in the item's directory that concern the watch, read on efsw's thread and spelled like its
         *        events (see eventSpelling): the item's and the ones it was renamed to. It only grows, a stale name
         *        lets an event through that onChange drops.
         */
        struct WatchedNames
        {
            std::mutex mutex{};
            std::set<std::string> names{};
            /**
             * @brief Lets every event of the directory through until the strand settled a rename of the item, see
             *        isRenamedAgain.
             */
            bool renaming{false};

            bool concerns(DirectoryEvent const& event)
            {
                std::scoped_lock lock{mutex};
                if (names.contains(event.filename))
                    return true;
                if (event.action != FileAction::Moved || !names.contains(event.oldFilename))
                    return renaming;
                // The strand follows the rename later, what happens at the new name meanwhile has to reach it.
                names.insert(event.filename);
                renaming = true;
                return true;
            }

            void add(std::string const& name)
            {
                std::scoped_lock lock{mutex};
                names.insert(name);
            }

            void renameSettled()
            {
                std::scoped_lock lock{mutex};
                renaming = false;
            }
        };

        Ids::WatchId watchId;
        std::filesystem::path item;
        DiskPlace place;
        bool isDirectory;
        std::function<bool(Ids::WatchId const&, std::filesystem::path const&)> mayFollow;
        ChangeReporter reporter;
        /** @brief The suffix of downloads in progress, see Persistence::effectiveTempFileSuffix. */
        std::string effectiveDownloadTempSuffix;
        std::shared_ptr<WatchedNames> watchedNames{std::make_shared<WatchedNames>()};
        /** @brief Ends the hold of pendingRename or pendingRemoval, only one of them is held at a time. */
        boost::asio::steady_timer holdTimer;
        /** @brief Tells the end of the current hold from the ones a later hold replaced. */
        std::uint64_t holdGeneration{0};
        /** @brief Ends the item's recursive watch a while after the item was deleted, see endItemWatchSoon. */
        boost::asio::steady_timer itemWatchEndTimer;
        std::optional<PendingRename> pendingRename{};
        /**
         * @brief The removal of the item, held back until it is clear the item does not come back: editors delete
         *        and write anew, Windows reports a case-only rename as a removal first.
         */
        std::optional<DirectoryChange> pendingRemoval{};
        std::optional<FollowedRename> followedRename{};
        std::optional<DirectoryWatcher> watcher{};
        std::optional<InstanceWatch> directoryWatch{};
        std::optional<InstanceWatch> itemWatch{};

        State(
            Ids::WatchId id,
            std::filesystem::path watchedItem,
            bool itemIsDirectory,
            std::function<bool(Ids::WatchId const&, std::filesystem::path const&)> mayFollowRename,
            boost::asio::strand<boost::asio::any_io_executor> const& strand,
            ChangeSink sink,
            std::string const& downloadTempSuffix
        )
            : watchId{std::move(id)}
            , item{std::move(watchedItem)}
            , place{DiskPlace::of(item)}
            , isDirectory{itemIsDirectory}
            , mayFollow{std::move(mayFollowRename)}
            , reporter{strand, std::move(sink), downloadTempSuffix}
            , effectiveDownloadTempSuffix{Persistence::effectiveTempFileSuffix(downloadTempSuffix)}
            , holdTimer{strand}
            , itemWatchEndTimer{strand}
        {
            watchedNames->names.insert(eventSpelling(Paths::nativeToUtf8(item.filename())));
        }

        /**
         * @brief Drops noise on efsw's thread, see isTransientChange.
         */
        DirectoryWatcher::EventFilter dropsNoise() const
        {
            return [suffix = effectiveDownloadTempSuffix](DirectoryEvent const& event)
            {
                return !isTransientChange(event.filename, event.oldFilename, suffix);
            };
        }

        /**
         * @brief Lets through on efsw's thread only the events of the item's directory that concern the item, the
         *        directory can be a busy one like the home directory.
         */
        DirectoryWatcher::EventFilter concernsItem() const
        {
            return [dropsNoise = dropsNoise(), names = watchedNames](DirectoryEvent const& event)
            {
                return dropsNoise(event) && names->concerns(event);
            };
        }

        void onChange(DirectoryChange const& reported)
        {
            const auto change = spelledLikeKnown(reported);
            const auto path = change.path();
            const auto oldPath = change.oldPath();
            const bool appeared = change.action == FileAction::Added || change.action == FileAction::Moved;
            if (change.action == FileAction::Moved && oldPath == item && path.parent_path() == item.parent_path())
                return holdRename(change);
            if (appeared && path == item)
                return onItemAppeared(change);
            if (appeared && mayReturnTo(path))
                return returnTo(change);
            if (pendingRename && change.action == FileAction::Moved && oldPath == pendingRename->change.path() &&
                path.parent_path() == item.parent_path())
            {
                auto renamedAgain = pendingRename->change;
                renamedAgain.filename = change.filename;
                return holdRename(renamedAgain);
            }
            // Windows reports the old name of a rename as removed too.
            if (pendingRename && change.action == FileAction::Deleted && path == item)
                return;
            if (pendingRename && change.action == FileAction::Added && isRenamedAgain(path))
            {
                auto renamedAgain = pendingRename->change;
                renamedAgain.filename = change.filename;
                watchedNames->add(change.filename);
                return holdRename(renamedAgain);
            }
            if (pendingRename && isDirectory && path != item && isAtOrBelow(item, path))
                return pendingRename->changesAtNewName.push_back(movedAlong(change, pendingRename->change.path()));
            if (pendingRename && isAtOrBelow(pendingRename->change.path(), path))
                return pendingRename->changesAtNewName.push_back(change);
            if (change.action == FileAction::Deleted && path == item)
                return holdRemoval(change);
            if (isChangeOfItem(item, path, oldPath))
                reporter.report(change);
        }

        /**
         * @brief @p change with the names of the item and of where the watch followed it from spelled like the watch
         *        knows them, where the event spells them differently (see eventSpelling).
         */
        DirectoryChange spelledLikeKnown(DirectoryChange change) const
        {
            respell(change, item);
            if (followedRename)
                respell(change, followedRename->from);
            return change;
        }

        /**
         * @brief Spells the name of @p known in @p change like @p known, where the change is in its directory and
         *        spells the name like the events do.
         */
        static void respell(DirectoryChange& change, std::filesystem::path const& known)
        {
            const auto knownName = Paths::nativeToUtf8(known.filename());
            const auto eventName = eventSpelling(knownName);
            if (eventName == knownName || change.path().parent_path() != known.parent_path())
                return;
            if (change.filename == eventName)
                change.filename = knownName;
            if (change.oldFilename == eventName)
                change.oldFilename = knownName;
        }

        /**
         * @brief @p change inside the renamed item as it happened at @p renamed: the old recursive watch follows the
         *        moved directory but keeps reporting its old path.
         */
        DirectoryChange movedAlong(DirectoryChange const& change, std::filesystem::path const& renamed) const
        {
            auto moved = change;
            moved.directory =
                (renamed / change.directory.lexically_normal().lexically_relative(item)).lexically_normal();
            // A move between two directories of the item names where it came from in full.
            if (const auto oldFilename = Paths::nativeFromUtf8(change.oldFilename);
                oldFilename.is_absolute() && isAtOrBelow(item, oldFilename))
                moved.oldFilename = Paths::nativeToUtf8(
                    (renamed / oldFilename.lexically_normal().lexically_relative(item)).lexically_normal()
                );
            std::error_code statusError;
            moved.isDirectory = std::filesystem::is_directory(moved.path(), statusError);
            return moved;
        }

        /**
         * @brief Whether @p path, added beside the item while its rename settles, is the item renamed once more:
         *        FSEvents merges the records of a name renamed to and away again, so efsw reports the last name as
         *        added.
         */
        bool isRenamedAgain(std::filesystem::path const& path) const
        {
            if (path.parent_path() != item.parent_path() || !place.self)
                return false;
            return diskIdentityOf(path) == place.self && diskIdentityOf(pendingRename->change.path()) != place.self;
        }

        /**
         * @brief Ends the item's recursive watch once its last changes had the settle time to arrive: moved out of
         *        its directory reads as deleted, and the watch would go on reporting the moved directory's content
         *        under the old path. On macOS every watch has a stream of its own, the removal of the item can arrive
         *        from its directory's stream before the removals of its content from its own.
         */
        void endItemWatchSoon()
        {
            itemWatchEndTimer.expires_after(changeSettleTime);
            itemWatchEndTimer.async_wait(
                [weak = weak_from_this()](boost::system::error_code const& error)
                {
                    if (error == boost::asio::error::operation_aborted)
                        return;
                    if (const auto self = weak.lock())
                        self->itemWatch.reset();
                }
            );
        }

        void holdRename(DirectoryChange const& change)
        {
            pendingRemoval.reset();
            followedRename.reset();
            pendingRename = PendingRename{.change = change};
            hold();
        }

        void holdRemoval(DirectoryChange const& change)
        {
            endItemWatchSoon();
            pendingRemoval = change;
            hold();
        }

        /**
         * @brief Acts on the pending rename or removal once nothing replaced it for the settle time.
         */
        void hold()
        {
            const auto generation = ++holdGeneration;
            holdTimer.expires_after(changeSettleTime);
            holdTimer.async_wait(
                [weak = weak_from_this(), generation](boost::system::error_code const& error)
                {
                    if (error == boost::asio::error::operation_aborted)
                        return;
                    const auto self = weak.lock();
                    // A wait that completed before a later hold restarted the timer is not the last one.
                    if (!self || self->holdGeneration != generation)
                        return;
                    if (self->pendingRename)
                        return self->followRename();
                    self->reportRemoval();
                }
            );
        }

        void endHold()
        {
            ++holdGeneration;
            holdTimer.cancel();
            pendingRename.reset();
            pendingRemoval.reset();
        }

        /**
         * @brief The item is there again, created, saved over or moved back: an editor moved the original aside or
         *        removed it to write a new one, rather than the user renaming or deleting it.
         */
        void onItemAppeared(DirectoryChange const& change)
        {
            if (pendingRename)
                watchedNames->renameSettled();
            endHold();
            isDirectory = change.isDirectory;
            place = DiskPlace::of(item);
            watchItemDirectory();
            reporter.report(change);
        }

        /**
         * @brief Whether the item appearing at @p path takes the watch back to the name it last followed a rename
         *        from, see renameReturnTime.
         */
        bool mayReturnTo(std::filesystem::path const& path) const
        {
            return followedRename && path == followedRename->from &&
                std::chrono::steady_clock::now() < followedRename->until && mayFollow(watchId, path);
        }

        /**
         * @brief The item appeared again at the name the watch followed it from: an editor moved the original aside
         *        and wrote the new file after the settle time. The watch goes back, a rename back and the new content
         *        are reported. A held removal of the name it followed to is moot then.
         */
        void returnTo(DirectoryChange const& change)
        {
            Log::info(
                "FileTracking: watch {} returns from '{}' to '{}', the item was written there anew",
                watchId.value(),
                Paths::nativeToUtf8(item),
                Paths::nativeToUtf8(change.path())
            );
            const auto followed = std::exchange(item, change.path());
            followedRename.reset();
            reporter.report(DirectoryChange{
                .action = FileAction::Moved,
                .directory = item.parent_path(),
                .filename = Paths::nativeToUtf8(item.filename()),
                .oldFilename = Paths::nativeToUtf8(followed.filename()),
                .isDirectory = isDirectory,
            });
            onItemAppeared(change);
        }

        void reportRemoval()
        {
            if (!pendingRemoval)
                return;
            const auto removal = *std::exchange(pendingRemoval, std::nullopt);
            reporter.report(removal);
        }

        void followRename()
        {
            if (!pendingRename)
                return;
            auto rename = *std::exchange(pendingRename, std::nullopt);
            watchedNames->renameSettled();
            // Onto another watch, which reports what arrives there, or onto a name whose changes are noise: for this
            // one the item is gone.
            if (reporter.isNoise(rename.change.filename) || !mayFollow(watchId, rename.change.path()))
            {
                Log::info(
                    "FileTracking: watch {} does not follow '{}' to '{}', it overlaps another watch or is temporary",
                    watchId.value(),
                    Paths::nativeToUtf8(item),
                    Paths::nativeToUtf8(rename.change.path())
                );
                // The recursive watch moved along with the directory and would report its content under the old path.
                itemWatch.reset();
                return reporter.report(DirectoryChange{
                    .action = FileAction::Deleted,
                    .directory = item.parent_path(),
                    .filename = Paths::nativeToUtf8(item.filename()),
                    .oldFilename = {},
                    .isDirectory = isDirectory,
                });
            }
            Log::info(
                "FileTracking: watch {} follows '{}' to '{}'",
                watchId.value(),
                Paths::nativeToUtf8(item),
                Paths::nativeToUtf8(rename.change.path())
            );
            followedRename = FollowedRename{
                .from = std::exchange(item, rename.change.path()),
                .until = std::chrono::steady_clock::now() + renameReturnTime,
            };
            place = DiskPlace::of(item);
            watchItemDirectory();
            reporter.report(rename.change);
            for (auto const& change : rename.changesAtNewName)
                onChange(change);
        }

        /**
         * @brief Watches the item recursively when it is a directory, anew: the watch of a directory that was
         *        replaced or renamed reports nothing or the old paths.
         */
        void watchItemDirectory()
        {
            itemWatchEndTimer.cancel();
            itemWatch.reset();
            if (!isDirectory)
                return;
            itemWatch = watcher->add(item, /*recursive=*/true, dropsNoise());
            if (!itemWatch)
                Log::warn("FileTracking: watch {} cannot watch '{}' again", watchId.value(), Paths::nativeToUtf8(item));
        }
    };

    std::expected<LocalItemWatch, std::string> LocalItemWatch::open(
        Ids::WatchId watchId,
        std::filesystem::path const& item,
        boost::asio::strand<boost::asio::any_io_executor> strand,
        ChangeSink sink,
        std::string const& downloadTempSuffix,
        std::function<bool(Ids::WatchId const&, std::filesystem::path const&)> mayFollow
    )
    {
        std::error_code typeError;
        const bool isDirectory = std::filesystem::is_directory(item, typeError);
        auto state = std::make_shared<State>(
            std::move(watchId), item, isDirectory, std::move(mayFollow), strand, std::move(sink), downloadTempSuffix
        );
        state->watcher.emplace(
            std::move(strand),
            [weak = std::weak_ptr<State>{state}](DirectoryChange const& change)
            {
                if (const auto self = weak.lock())
                    self->onChange(change);
            }
        );

        state->directoryWatch = state->watcher->add(item.parent_path(), /*recursive=*/false, state->concernsItem());
        if (isDirectory)
            state->itemWatch = state->watcher->add(item, /*recursive=*/true, state->dropsNoise());
        if (!state->directoryWatch || (isDirectory && !state->itemWatch))
            return std::unexpected{
                Utility::localizedMessage("backend.fileTracking.watchFailed", Paths::nativeToUtf8(item))
            };
        return LocalItemWatch{std::move(state)};
    }

    LocalItemWatch::LocalItemWatch(std::shared_ptr<State> state)
        : state_{std::move(state)}
    {}

    LocalItemWatch::~LocalItemWatch() = default;
    LocalItemWatch::LocalItemWatch(LocalItemWatch&&) noexcept = default;
    LocalItemWatch& LocalItemWatch::operator=(LocalItemWatch&&) noexcept = default;

    Ids::WatchId const& LocalItemWatch::watchId() const
    {
        return state_->watchId;
    }

    std::filesystem::path const& LocalItemWatch::item() const
    {
        return state_->item;
    }

    DiskPlace const& LocalItemWatch::place() const
    {
        return state_->place;
    }

    std::filesystem::path LocalItemWatch::directory() const
    {
        return state_->item.parent_path();
    }

    bool LocalItemWatch::isDirectory() const
    {
        return state_->isDirectory;
    }

    void LocalItemWatch::handleChange(DirectoryChange const& change)
    {
        state_->onChange(change);
    }
}
