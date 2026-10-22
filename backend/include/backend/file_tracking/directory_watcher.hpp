#pragma once

#include <backend/file_tracking/directory_watch_service.hpp>
#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/file_action.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace FileTracking
{
    /**
     * @brief One change of a watched directory, as it reaches the strand of its DirectoryWatcher.
     */
    struct DirectoryChange
    {
        FileAction action;
        /**
         * @brief The directory of the change as the watch reports it, it may end with a separator. At and above the
         *        watched directory it is spelled like that, see DirectoryWatcher::add.
         */
        std::filesystem::path directory;
        /** @brief UTF-8, spelled like efsw reports it, see eventSpelling. */
        std::string filename;
        /** @brief The previous name of a move, UTF-8 and spelled like filename, empty otherwise. */
        std::string oldFilename;
        /** @brief Whether path() was a directory when the watch reported the change. */
        bool isDirectory;

        std::filesystem::path path() const;

        /** @brief Where a move came from, empty for anything else. */
        std::filesystem::path oldPath() const;
    };

    /**
     * @brief Watches directories through the DirectoryWatchService of the process and hands their changes to its
     *        handler on its strand. Destroying it stops the handler, the subscriptions end with their handles.
     */
    class DirectoryWatcher
    {
      public:
        using ChangeHandler = std::function<void(DirectoryChange const& change)>;

        /**
         * @brief Decides on efsw's thread whether an event reaches the strand. Called for every event of every
         *        directory watched with it, it must be fast and thread safe.
         */
        using EventFilter = std::function<bool(DirectoryEvent const& event)>;

        /**
         * @param strand Runs @p onChange.
         * @param onChange Receives the changes of every directory added.
         */
        DirectoryWatcher(boost::asio::strand<boost::asio::any_io_executor> strand, ChangeHandler onChange);
        ~DirectoryWatcher();
        DirectoryWatcher(DirectoryWatcher const&) = delete;
        DirectoryWatcher& operator=(DirectoryWatcher const&) = delete;
        DirectoryWatcher(DirectoryWatcher&&) noexcept;
        DirectoryWatcher& operator=(DirectoryWatcher&&) noexcept;

        /**
         * @brief Watches @p directory, an existing absolute path spelled like the changes should report it, for the
         *        changes made from now on. Below it the changes keep the spelling of the events, see eventSpelling. On
         *        macOS a modification of what is gone once it arrives is dropped, efsw reports one before every removal
         *        there.
         * @param filter Drops the events it refuses before they are looked at further, nothing passes them all.
         * @return The handle that ends the watch, nothing when it was rejected.
         */
        std::optional<InstanceWatch>
        add(std::filesystem::path const& directory, bool recursive, EventFilter filter = {});

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
