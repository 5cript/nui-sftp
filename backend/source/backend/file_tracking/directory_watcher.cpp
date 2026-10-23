#include <backend/file_tracking/directory_watcher.hpp>
#include <backend/file_tracking/directory_watch_service.hpp>
#include <backend/file_tracking/event_spelling.hpp>
#include <paths/native.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>

#ifdef __APPLE__
#    include <sys/mount.h>
#    include <sys/stat.h>
#endif

#include <chrono>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief Gives the directory of an event the spelling of the watched directory where the event spells it
         *        differently (see eventSpelling). What lies below the watched directory keeps the event's spelling.
         */
        class WatchedSpelling
        {
          public:
            explicit WatchedSpelling(std::filesystem::path const& directory)
                : watchedSpelling_{Paths::nativeToUtf8(directory.has_filename() ? directory : directory.parent_path())}
                , eventSpelling_{eventSpelling(watchedSpelling_)}
            {}

            std::filesystem::path of(std::string const& eventDirectory) const
            {
                // Only macOS spells events differently, its separator ends the watched part.
                const bool spelledDifferently = eventSpelling_ != watchedSpelling_ &&
                    eventDirectory.starts_with(eventSpelling_) &&
                    (eventDirectory.size() == eventSpelling_.size() || eventDirectory[eventSpelling_.size()] == '/');
                if (!spelledDifferently)
                    return Paths::nativeFromUtf8(eventDirectory);
                return Paths::nativeFromUtf8(watchedSpelling_ + eventDirectory.substr(eventSpelling_.size()));
            }

          private:
            std::string watchedSpelling_;
            std::string eventSpelling_;
        };

#ifdef __APPLE__
        /**
         * @brief Tells the changes FSEvents reports although they were made before the watch began: a new stream
         *        also delivers what happened in the moments before it started.
         */
        class ReplayFilter
        {
          public:
            explicit ReplayFilter(std::filesystem::path const& directory)
                : watchedSince_{std::chrono::system_clock::now()}
                , filtersUntil_{
                      hasExactChangeTimes(directory) ? std::chrono::steady_clock::now() + replayTime
                                                     : std::chrono::steady_clock::time_point{}
                  }
            {}

            bool isReplay(DirectoryEvent const& event) const
            {
                // Whatever the watch has to report renews the entry's status change time, renames and moves too.
                if (event.action == FileAction::Deleted || std::chrono::steady_clock::now() >= filtersUntil_)
                    return false;
                const auto path = Paths::nativeFromUtf8(event.directory) / Paths::nativeFromUtf8(event.filename);
                struct stat status{};
                if (::lstat(path.c_str(), &status) != 0)
                    return false;
                const auto changed = std::chrono::system_clock::time_point{
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::seconds{status.st_ctimespec.tv_sec} +
                        std::chrono::nanoseconds{status.st_ctimespec.tv_nsec}
                    )
                };
                if (changed >= watchedSince_)
                    return false;
                Log::debug("FileTracking: '{}' changed before its watch began", Paths::nativeToUtf8(path));
                return true;
            }

          private:
            /**
             * @brief How long after a watch began FSEvents may still report what happened before.
             */
            static constexpr std::chrono::seconds replayTime{2};

            /**
             * @brief Whether the volume of @p directory keeps status change times to the nanosecond, FAT and network
             *        volumes round them or take them from another clock.
             */
            static bool hasExactChangeTimes(std::filesystem::path const& directory)
            {
                struct statfs volume{};
                if (::statfs(directory.c_str(), &volume) != 0)
                    return false;
                const std::string_view type{volume.f_fstypename};
                return type == "apfs" || type == "hfs";
            }

            std::chrono::system_clock::time_point watchedSince_;
            std::chrono::steady_clock::time_point filtersUntil_;
        };
#else
        /**
         * @brief Only FSEvents reports changes made before a watch began.
         */
        class ReplayFilter
        {
          public:
            explicit ReplayFilter(std::filesystem::path const& /*directory*/)
            {}

            bool isReplay(DirectoryEvent const& /*event*/) const
            {
                return false;
            }
        };
#endif

        /**
         * @brief Whether @p action modified what is gone by now (@p status) and means nothing: efsw's FSEvents watcher
         *        reports a modification before every removal. Elsewhere it can be an edit just before a rename, which
         *        the tracker uploads to the new name.
         */
        bool modifiedWhatIsGone(
            [[maybe_unused]] FileAction action,
            [[maybe_unused]] std::filesystem::file_status const& status
        )
        {
#ifdef __APPLE__
            return action == FileAction::Modified && status.type() == std::filesystem::file_type::not_found;
#else
            return false;
#endif
        }

        /**
         * @brief Hands @p event, in @p directory, to the handler on @p strand once it was looked at what the event
         *        names.
         */
        void handOn(
            boost::asio::strand<boost::asio::any_io_executor> const& strand,
            std::weak_ptr<DirectoryWatcher::ChangeHandler> const& weakHandler,
            DirectoryEvent const& event,
            std::filesystem::path const& directory
        )
        {
            if (weakHandler.expired())
                return;
            std::error_code statusError;
            const auto status = std::filesystem::status(directory / Paths::nativeFromUtf8(event.filename), statusError);
            if (modifiedWhatIsGone(event.action, status))
                return;
            boost::asio::post(
                strand,
                [weakHandler,
                 change = DirectoryChange{
                     .action = event.action,
                     .directory = directory,
                     .filename = event.filename,
                     .oldFilename = event.oldFilename,
                     .isDirectory = std::filesystem::is_directory(status),
                 }]()
                {
                    if (const auto onChange = weakHandler.lock())
                        (*onChange)(change);
                }
            );
        }
    }

    std::filesystem::path DirectoryChange::path() const
    {
        if (filename.empty())
            return directory.has_filename() ? directory : directory.parent_path();
        return directory / Paths::nativeFromUtf8(filename);
    }

    std::filesystem::path DirectoryChange::oldPath() const
    {
        return oldFilename.empty() ? std::filesystem::path{} : directory / Paths::nativeFromUtf8(oldFilename);
    }

    struct DirectoryWatcher::Implementation
    {
        std::shared_ptr<DirectoryWatchService> service;
        /**
         * @brief Looks at what the events name, in their order. Not on efsw's thread, which Windows outpaces when a
         *        watched directory is deleted, and not on the strand, which serves the RPCs too.
         */
        boost::asio::strand<boost::asio::any_io_executor> worker;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        std::shared_ptr<ChangeHandler> onChange;
    };

    DirectoryWatcher::DirectoryWatcher(boost::asio::strand<boost::asio::any_io_executor> strand, ChangeHandler onChange)
        : impl_{std::make_unique<Implementation>(Implementation{
              .service = DirectoryWatchService::shared(),
              .worker = boost::asio::make_strand(strand.get_inner_executor()),
              .strand = std::move(strand),
              .onChange = std::make_shared<ChangeHandler>(std::move(onChange)),
          })}
    {}

    DirectoryWatcher::~DirectoryWatcher() = default;
    DirectoryWatcher::DirectoryWatcher(DirectoryWatcher&&) noexcept = default;
    DirectoryWatcher& DirectoryWatcher::operator=(DirectoryWatcher&&) noexcept = default;

    std::optional<InstanceWatch>
    DirectoryWatcher::add(std::filesystem::path const& directory, bool recursive, EventFilter filter)
    {
        return impl_->service->subscribe(
            directory,
            recursive,
            [worker = impl_->worker,
             strand = impl_->strand,
             weakHandler = std::weak_ptr<ChangeHandler>{impl_->onChange},
             replays = ReplayFilter{directory},
             filter = std::move(filter),
             spelling = WatchedSpelling{directory}](DirectoryEvent const& event)
            {
                // Before the filter, which may keep state about the events it lets through. A missed change names
                // no entry to judge by.
                if (event.action != FileAction::Missed && (replays.isReplay(event) || (filter && !filter(event))))
                    return;
                boost::asio::post(
                    worker,
                    [strand, weakHandler, event, changedDirectory = spelling.of(event.directory)]()
                    {
                        handOn(strand, weakHandler, event, changedDirectory);
                    }
                );
            }
        );
    }
}
