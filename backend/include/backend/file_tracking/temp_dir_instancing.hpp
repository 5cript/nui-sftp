#pragma once

#include <backend/file_tracking/file_action.hpp>
#include <backend/file_tracking/instance_lock.hpp>
#include <backend/file_tracking/instance_watch.hpp>
#include <backend/file_tracking/reported_change.hpp>
#include <persistence/state/sftp_options.hpp>
#include <paths/paths.hpp>

#include <boost/asio/strand.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace FileTracking
{
    /**
     * @brief Manages one temporary directory instance used for file-change tracking.
     *
     * On construction a UUID-named subdirectory is created under @p tempRootDir,
     * and an exclusive instance lock file is acquired and a metadata file written
     * in its bookkeeping directory (see bookkeepingPaths()). Events inside the
     * bookkeeping directory are not forwarded.
     *
     * On destruction the instance does **not** delete its directory.  It writes a
     * "no longer alive" timestamp into the metadata file so a future scan (performed
     * by TempDirInstanceManager) can reclaim the space after the configured
     * retention period.
     *
     * File-change events inside the instance directory go to the sink of its first watch, see addWatch().
     *
     * The class is non-copyable and non-movable because it owns OS-level resources
     * (lock file descriptor, its directory watches).
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
         * @param strand   Strand the changes of the watched directories are handled
         *                 and reported on.
         *
         * @throws std::runtime_error if the instance directory cannot be created or
         *         the lock file cannot be acquired.
         */
        TemporaryDirectoryInstance(Config config, boost::asio::strand<boost::asio::any_io_executor> strand);

        /**
         * @brief Stops reporting changes and writes the death timestamp to the
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
         * @brief Watch a directory and return a move-only RAII handle.
         *
         * The watch is removed automatically when the returned InstanceWatch is
         * destroyed or release() is called on it.
         *
         * @param path      Absolute path or path relative to instanceDir().  Must
         *                  resolve to a location inside instanceDir().
         * @param recursive Whether to recurse into subdirectories.
         * @param downloadTempSuffix The suffix downloads into the instance write under until complete, those
         *                  files and their final rename are not mirrored to the remote side.
         * @param sink      Receives the changes. The first watch decides the suffix and the sink, later ones share
         *                  its event stream.
         * @return A valid InstanceWatch on success, or std::nullopt if the path is
         *         outside instanceDir(), does not exist, or efsw rejected the watch.
         */
        std::optional<InstanceWatch> addWatch(
            std::filesystem::path const& path,
            bool recursive = true,
            std::string downloadTempSuffix = std::string{Persistence::defaultTempFileSuffix},
            ChangeSink sink = [](ReportedChange const&) {}
        );

        /**
         * @brief Records in the metadata that the instance holds a download of @p remotePath, beside the fields older
         *        builds read.
         * @return An error when the metadata cannot be written or cleanupNow() ran.
         */
        std::expected<void, std::string> recordOrigin(Paths::RemotePath const& remotePath);

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
