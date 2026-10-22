#include <backend/file_tracking/directory_watch_service.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>

#include <efsw/efsw.hpp>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace FileTracking
{
    namespace
    {
        FileAction toFileAction(efsw::Action action)
        {
            switch (action)
            {
                case efsw::Actions::Add:
                    return FileAction::Added;
                case efsw::Actions::Modified:
                    return FileAction::Modified;
                case efsw::Actions::Delete:
                    return FileAction::Deleted;
                case efsw::Actions::Moved:
                    return FileAction::Moved;
                default:
                    return FileAction::Modified;
            }
        }

        /**
         * @brief @p directory ending with a separator, so spellings with and without one compare equal.
         */
        std::filesystem::path directoryKey(std::filesystem::path const& directory)
        {
            return (directory / "").lexically_normal();
        }

        /**
         * @brief Whether watches without recursion share one efsw watcher, see DirectoryWatchService.
         */
#ifdef __APPLE__
        constexpr bool sharesWatcher = false;
#else
        constexpr bool sharesWatcher = true;
#endif
    }

    struct DirectoryWatchService::Implementation : public efsw::FileWatchListener
    {
        /**
         * @brief The efsw watcher of one watch, which delivers to the subscriptions of @p key.
         */
        struct OwnWatcher : public efsw::FileWatchListener
        {
            Implementation& service;
            std::filesystem::path key;
            /** @brief Declared last, so its thread ends before the listener goes. */
            efsw::FileWatcher watcher{};

            OwnWatcher(Implementation& owner, std::filesystem::path watchedKey)
                : service{owner}
                , key{std::move(watchedKey)}
            {}

            void handleFileAction(
                efsw::WatchID /*watchId*/,
                std::string const& dir,
                std::string const& filename,
                efsw::Action action,
                std::string const& oldFilename
            ) override
            {
                service.dispatch(
                    [this](std::map<std::filesystem::path, Watch> const& watches) -> Watch const*
                    {
                        // Until it is registered or after it was discarded the key may belong to another watch.
                        const auto found = watches.find(key);
                        return found != watches.end() && found->second.ownWatcher.get() == this ? &found->second
                                                                                                : nullptr;
                    },
                    dir,
                    filename,
                    action,
                    oldFilename
                );
            }
        };

        struct Watch
        {
            efsw::WatchID efswId;
            bool recursive;
            std::map<std::int64_t, DirectoryEventSink> subscriptions;
            /** @brief The watcher of the watch, nothing for one on the shared watcher. */
            std::unique_ptr<OwnWatcher> ownWatcher{};
        };

        /** @brief Serialises subscribe and unsubscribe. */
        std::mutex registrationMutex{};
        /**
         * @brief Guards the maps for efsw's threads. Never held around a call into efsw, which holds its own lock
         *        while it calls handleFileAction.
         */
        std::mutex dispatchMutex{};
        std::map<std::filesystem::path, Watch> watches{};
        /** @brief The directories of the watches on the shared watcher. */
        std::map<efsw::WatchID, std::filesystem::path> directoryOfWatch{};
        std::map<std::int64_t, std::filesystem::path> directoryOfSubscription{};
        std::int64_t nextSubscriptionId{0};
        /**
         * @brief Shared by the watches without recursion. Declared last, so its thread ends before anything it calls
         *        into goes.
         */
        efsw::FileWatcher watcher{};

        Implementation() = default;
        Implementation(Implementation const&) = delete;
        Implementation& operator=(Implementation const&) = delete;
        Implementation(Implementation&&) = delete;
        Implementation& operator=(Implementation&&) = delete;

        ~Implementation() override
        {
            // Their threads deliver into the maps, so they end while those still exist.
            std::vector<std::unique_ptr<OwnWatcher>> ownWatchers;
            {
                std::scoped_lock lock{dispatchMutex};
                for (auto& watch : std::views::values(watches))
                    if (watch.ownWatcher)
                        ownWatchers.push_back(std::move(watch.ownWatcher));
            }
        }

        void handleFileAction(
            efsw::WatchID watchId,
            std::string const& dir,
            std::string const& filename,
            efsw::Action action,
            std::string const& oldFilename
        ) override
        {
            dispatch(
                [this, watchId](std::map<std::filesystem::path, Watch> const& known) -> Watch const*
                {
                    const auto directory = directoryOfWatch.find(watchId);
                    return directory == directoryOfWatch.end() ? nullptr : &known.at(directory->second);
                },
                dir,
                filename,
                action,
                oldFilename
            );
        }

        /**
         * @brief Hands an event to the subscriptions of the watch @p findWatch picks, it is called with the dispatch
         *        mutex held.
         */
        void dispatch(
            std::function<Watch const*(std::map<std::filesystem::path, Watch> const&)> const& findWatch,
            std::string const& dir,
            std::string const& filename,
            efsw::Action action,
            std::string const& oldFilename
        )
        {
            std::vector<DirectoryEventSink> sinks;
            {
                std::scoped_lock lock{dispatchMutex};
                const auto* watch = findWatch(watches);
                if (!watch)
                    return;
                std::ranges::copy(watch->subscriptions | std::views::values, std::back_inserter(sinks));
            }
            const DirectoryEvent event{
                .action = toFileAction(action),
                .directory = dir,
                .filename = filename,
                .oldFilename = oldFilename,
            };
            for (auto const& sink : sinks)
                sink(event);
        }

        /**
         * @brief Watches @p directory with the efsw watcher @p efswWatcher, which reports to @p listener.
         * @return The efsw watch, negative when efsw rejected it.
         */
        static efsw::WatchID watchWithEfsw(
            efsw::FileWatcher& efswWatcher,
            efsw::FileWatchListener& listener,
            std::filesystem::path const& directory,
            bool recursive
        )
        {
            // A move between two of its directories comes as one move with the absolute old path, where the platform
            // tells that, not as a deletion and an addition. Not on Windows: efsw holds every deletion back to pair it
            // and drops the held ones when the watched directory goes.
            std::vector<efsw::WatcherOption> options;
#ifndef _WIN32
            if (recursive)
                options.emplace_back(efsw::Options::ReportCrossDirectoryMoves, 1);
#endif
            // efsw takes UTF-8, a narrow string is the ANSI code page on Windows.
            const auto efswId = efswWatcher.addWatch(Paths::nativeToUtf8(directory), &listener, recursive, options);
            if (efswId < 0)
            {
                Log::warn(
                    "DirectoryWatchService: efsw rejected the watch of '{}': {}",
                    Paths::nativeToUtf8(directory),
                    efsw::Errors::Log::getLastErrorLog()
                );
                return efswId;
            }
            efswWatcher.watch();
            return efswId;
        }

        /**
         * @brief Subscribes @p sink to the watch of @p key, which exists. Called with the dispatch mutex held.
         * @return Nothing when the watch is the other way (with or without recursion).
         */
        std::optional<InstanceWatch> joinWatch(
            std::weak_ptr<DirectoryWatchService> service,
            std::filesystem::path const& key,
            std::filesystem::path const& directory,
            bool recursive,
            DirectoryEventSink sink
        )
        {
            auto& watch = watches.at(key);
            // Watches never nest (see LocalWatchManager), so nobody needs a directory watched both ways.
            if (watch.recursive != recursive)
            {
                Log::warn(
                    "DirectoryWatchService: '{}' is watched {} already",
                    Paths::nativeToUtf8(directory),
                    watch.recursive ? "recursively" : "without recursion"
                );
                return std::nullopt;
            }
            const auto subscriptionId = nextSubscriptionId++;
            watch.subscriptions.emplace(subscriptionId, std::move(sink));
            directoryOfSubscription.emplace(subscriptionId, key);
            return InstanceWatch{std::move(service), subscriptionId, directory};
        }
    };

    std::shared_ptr<DirectoryWatchService> DirectoryWatchService::shared()
    {
        static std::mutex mutex;
        static std::weak_ptr<DirectoryWatchService> instance;
        std::scoped_lock lock{mutex};
        if (auto service = instance.lock())
            return service;
        auto service = std::make_shared<DirectoryWatchService>();
        instance = service;
        return service;
    }

    DirectoryWatchService::DirectoryWatchService()
        : impl_{std::make_unique<Implementation>()}
    {}

    DirectoryWatchService::~DirectoryWatchService() = default;

    std::optional<InstanceWatch> DirectoryWatchService::subscribe(
        std::filesystem::path const& directory,
        bool recursive,
        DirectoryEventSink sink
    )
    {
        auto& impl = *impl_;
        const auto key = directoryKey(directory);
        {
            std::scoped_lock registration{impl.registrationMutex};
            {
                std::scoped_lock lock{impl.dispatchMutex};
                if (impl.watches.contains(key))
                    return impl.joinWatch(weak_from_this(), key, directory, recursive, std::move(sink));
            }
            if (!recursive && sharesWatcher)
            {
                const auto efswId = Implementation::watchWithEfsw(impl.watcher, impl, directory, recursive);
                if (efswId < 0)
                    return std::nullopt;
                std::scoped_lock lock{impl.dispatchMutex};
                impl.watches.emplace(
                    key, Implementation::Watch{.efswId = efswId, .recursive = recursive, .subscriptions = {}}
                );
                impl.directoryOfWatch.emplace(efswId, key);
                return impl.joinWatch(weak_from_this(), key, directory, recursive, std::move(sink));
            }
        }

        // The walk below the directory takes as long as the tree is large, no lock of the service is held meanwhile.
        auto ownWatcher = std::make_unique<Implementation::OwnWatcher>(impl, key);
        const auto efswId = Implementation::watchWithEfsw(ownWatcher->watcher, *ownWatcher, directory, recursive);
        if (efswId < 0)
            return std::nullopt;
        std::optional<InstanceWatch> subscription;
        {
            std::scoped_lock registration{impl.registrationMutex};
            std::scoped_lock lock{impl.dispatchMutex};
            // Another caller watched the directory meanwhile, its watch serves this one too.
            if (!impl.watches.contains(key))
                impl.watches.emplace(
                    key,
                    Implementation::Watch{
                        .efswId = efswId,
                        .recursive = recursive,
                        .subscriptions = {},
                        .ownWatcher = std::move(ownWatcher),
                    }
                );
            subscription = impl.joinWatch(weak_from_this(), key, directory, recursive, std::move(sink));
        }
        // A watcher nobody took ends here, outside the locks its thread takes.
        ownWatcher.reset();
        return subscription;
    }

    void DirectoryWatchService::unsubscribe(std::int64_t subscriptionId)
    {
        auto& impl = *impl_;
        std::unique_ptr<Implementation::OwnWatcher> unusedOwnWatcher;
        {
            std::scoped_lock registration{impl.registrationMutex};
            std::optional<efsw::WatchID> unused;
            {
                std::scoped_lock lock{impl.dispatchMutex};
                const auto directory = impl.directoryOfSubscription.find(subscriptionId);
                if (directory == impl.directoryOfSubscription.end())
                    return;
                const auto key = directory->second;
                impl.directoryOfSubscription.erase(directory);
                const auto watch = impl.watches.find(key);
                if (watch == impl.watches.end())
                    return;
                watch->second.subscriptions.erase(subscriptionId);
                if (!watch->second.subscriptions.empty())
                    return;
                if (watch->second.ownWatcher)
                    unusedOwnWatcher = std::move(watch->second.ownWatcher);
                else
                {
                    unused = watch->second.efswId;
                    impl.directoryOfWatch.erase(watch->second.efswId);
                }
                impl.watches.erase(watch);
            }
            if (unused)
                impl.watcher.removeWatch(*unused);
        }
        // Joins its thread, outside the locks that thread takes.
        unusedOwnWatcher.reset();
    }
}
