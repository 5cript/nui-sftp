#pragma once

#include <backend/file_tracking/instance_lock.hpp>
#include <backend/file_tracking/instance_watch.hpp>
#include <utility/describe.hpp>

#include <boost/asio/strand.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Nui
{
    class Window;
    class RpcHub;
}

namespace FileTracking
{
    /**
     * @brief Mirrors the efsw action values; used to produce the "action" field
     *        in onFileChanged RPC events.
     */
    enum class FileAction
    {
        Added,
        Modified,
        Deleted,
        Moved,
    };
    BOOST_DESCRIBE_ENUM(FileAction, Added, Modified, Deleted, Moved)

    // -------------------------------------------------------------------------

    /**
     * @brief Manages one temporary directory instance used for file-change tracking.
     *
     * On construction a UUID-named subdirectory is created under @p tempRootDir,
     * an exclusive instance lock file is acquired inside it, and a metadata file is
     * written.
     *
     * On destruction the instance does **not** delete its directory.  It writes a
     * "no longer alive" timestamp into the metadata file so a future scan (performed
     * by TempDirInstanceManager) can reclaim the space after the configured
     * retention period.
     *
     * File-change events inside the instance directory are forwarded to the frontend
     * via @c Nui::RpcHub under the event name
     * @c "FileTracking::<instanceId>::onFileChanged" with a JSON payload:
     * @code
     * {
     *   "action":      "Added" | "Modified" | "Deleted" | "Moved",
     *   "directory":   "/abs/path/to/dir/",
     *   "filename":    "changed-file.txt",
     *   "oldFilename": "previous-name.txt"   // only populated for Moved
     * }
     * @endcode
     *
     * The class is non-copyable and non-movable because it owns OS-level resources
     * (lock file descriptor, efsw watcher thread).
     */
    class TemporaryDirectoryInstance
    {
      public:
        /**
         * @brief Configuration passed to the constructor.
         */
        struct Config
        {
            /** @brief Root directory under which instance subdirectories are created. */
            std::filesystem::path tempRootDir;
        };

        /**
         * @brief Construct a new instance, create its directory, acquire the lock,
         *        and write the initial metadata.
         *
         * @param config   Directory root configuration.
         * @param strand   Strand used to serialise RPC event dispatch from the efsw
         *                 callback thread.
         * @param wnd      Nui window (reserved for future use).
         * @param hub      RPC hub used to push file-change events to the frontend.
         *
         * @throws std::runtime_error if the instance directory cannot be created or
         *         the lock file cannot be acquired.
         */
        TemporaryDirectoryInstance(
            Config config,
            boost::asio::strand<boost::asio::any_io_executor> strand,
            Nui::Window& wnd,
            Nui::RpcHub& hub
        );

        /**
         * @brief Stops the efsw watcher and writes the death timestamp to the
         *        metadata file, but does **not** delete the instance directory.
         */
        ~TemporaryDirectoryInstance();

        TemporaryDirectoryInstance(TemporaryDirectoryInstance const&) = delete;
        TemporaryDirectoryInstance& operator=(TemporaryDirectoryInstance const&) = delete;
        TemporaryDirectoryInstance(TemporaryDirectoryInstance&&) = delete;
        TemporaryDirectoryInstance& operator=(TemporaryDirectoryInstance&&) = delete;

        /**
         * @brief Returns the UUID string that identifies this instance.
         */
        std::string const& instanceId() const;

        /**
         * @brief Returns the absolute path to this instance's working directory.
         */
        std::filesystem::path instanceDir() const;

        /**
         * @brief Register an efsw watch and return a move-only RAII handle.
         *
         * The watch is removed automatically when the returned InstanceWatch is
         * destroyed or release() is called on it.  The efsw watcher is (re)started
         * after a successful watch is added.
         *
         * @param path      Absolute path or path relative to instanceDir().  Must
         *                  resolve to a location inside instanceDir().
         * @param recursive Whether to recurse into subdirectories.
         * @return A valid InstanceWatch on success, or std::nullopt if the path is
         *         outside instanceDir(), does not exist, or efsw rejected the watch.
         */
        std::optional<InstanceWatch> addWatch(std::filesystem::path const& path, bool recursive = true);

        /**
         * @brief Immediately delete the instance directory and invalidate this
         *        object.  No death timestamp is written.
         *
         * After this call isValid() returns false.
         */
        void cleanupNow();

        /**
         * @brief Returns false after cleanupNow() has been called.
         */
        bool isValid() const;

      private:
        void writeMetadata();
        void writeDeadTimestamp();

      private:
        struct Implementation;
        std::unique_ptr<Implementation> impl_;
    };
}
