#pragma once

#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/file_action.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace FileTracking
{
    /**
     * @brief One change as efsw reports it, the names UTF-8 on every platform.
     */
    struct DirectoryEvent
    {
        FileAction action;
        /**
         * @brief The directory of the change, efsw ends it with a separator. For Missed the directory whose changes
         *        were lost, with or without one.
         */
        std::string directory;
        /** @brief Empty for Missed, which concerns the directory. */
        std::string filename;
        /** @brief The previous name of a move, empty otherwise. */
        std::string oldFilename;
    };

    /**
     * @brief Receives the events of a subscription on efsw's thread, it must only hand them on (post to a strand).
     *        Anything slower than a stat holds up the events of every watch.
     */
    using DirectoryEventSink = std::function<void(DirectoryEvent const& event)>;

    /**
     * @brief The efsw watchers of the process. Directories watched without recursion share one watcher and its
     *        thread, except on macOS: there a watcher is one FSEvents stream, which efsw rebuilds whenever one of its
     *        watches comes or goes and drops the events on their way meanwhile. A recursive watch gets a watcher of
     *        its own: efsw holds its lock while it walks the tree to set the watch up and while it delivers an event,
     *        so a large tree would hold up the events of every other watch. A directory is watched once however many
     *        subscribe to it, all of them with or all without recursion. Thread-safe. Held in a std::shared_ptr, see
     *        shared().
     */
    class DirectoryWatchService : public std::enable_shared_from_this<DirectoryWatchService>
    {
      public:
        /**
         * @brief The service, created by the first caller. It lives as long as someone holds it.
         */
        static std::shared_ptr<DirectoryWatchService> shared();

        DirectoryWatchService();
        ~DirectoryWatchService();
        DirectoryWatchService(DirectoryWatchService const&) = delete;
        DirectoryWatchService& operator=(DirectoryWatchService const&) = delete;
        DirectoryWatchService(DirectoryWatchService&&) = delete;
        DirectoryWatchService& operator=(DirectoryWatchService&&) = delete;

        /**
         * @brief Delivers the changes in @p directory, and below it when @p recursive, to @p sink.
         * @param directory An existing absolute path spelled like the events should report it. A recursive watch
         *        walks it before this returns, without blocking other callers.
         * @return The handle that ends the subscription, nothing when efsw rejected the watch or @p directory is
         *         watched the other way (with or without recursion) already.
         */
        std::optional<InstanceWatch> subscribe(std::filesystem::path const& directory, bool recursive, DirectoryEventSink sink);

        /**
         * @brief Ends the subscription @p subscriptionId, the directory is no longer watched once nobody subscribes.
         */
        void unsubscribe(std::int64_t subscriptionId);

        /**
         * @brief Tells the subscriptions of the watch of @p directory, or of the recursive one above it, that the
         *        changes in @p directory were lost, the way efsw does. For tests, efsw cannot be made to lose changes
         *        on every platform.
         */
        void reportMissed(std::filesystem::path const& directory);

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
