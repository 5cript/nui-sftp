#pragma once

#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <ids/ids.hpp>
#include <shared_data/file_tracking/orphaned_copy.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace FileTracking
{
    /**
     * @brief Information snapshot for one tracked instance.
     */
    struct InstanceInfo
    {
        std::string instanceId;
        std::filesystem::path instanceDir;
    };

    namespace InstanceManagerDetail
    {
        /**
         * @brief The states (see fileStateOf) of every regular file and directory below @p instanceDirectory, paths
         *        canonical. Symbolic links and the bookkeeping directory are left out.
         * @param below Lists only this path and what lies below it, nothing when it is outside the instance or in its
         *        bookkeeping. Empty lists everything.
         */
        std::vector<FileState>
        fileStatesOf(std::filesystem::path const& instanceDirectory, std::filesystem::path const& below = {});

        /**
         * @brief @p path as content of @p instanceDirectory that may be removed: its parent directory resolved,
         *        its last component kept as it is, so a link there is removed itself.
         * @return An error when @p path is not below @p instanceDirectory once its parent is resolved, is the
         *         instance directory itself or lies in the bookkeeping directory.
         */
        std::expected<std::filesystem::path, std::string>
        removableContentPath(std::filesystem::path const& instanceDirectory, std::filesystem::path const& path);

        /**
         * @brief Whether @p instanceDirectory holds anything besides its bookkeeping directory. A directory that
         *        cannot be read counts as holding content.
         */
        bool hasContent(std::filesystem::path const& instanceDirectory);
    }

    /**
     * @brief What TempDirInstanceManager::discardContent did with the instance after removing the content.
     */
    enum class DiscardOutcome
    {
        InstanceKept,
        InstanceDestroyed,
    };

    /**
     * @brief Owns and tracks the TemporaryDirectoryInstance objects of every session.
     *
     * The manager also owns the dead-instance cleanup policy: the retention scan runs once at start (see
     * scheduleStartupCleanup), the user's Clean Up removes every orphaned directory at once (removeOrphaned).
     * Lives on its strand: every member must be called from it.
     */
    class TempDirInstanceManager
    {
      public:
        /**
         * @brief Construct the manager.
         *
         * @param strand                 Runs the instances' changes and the startup cleanup.
         * @param tempRootDir            Root directory under which instance
         *                               subdirectories are created.
         * @param deadInstanceRetentionHours
         *                               How long a dead instance directory is kept
         *                               before manualCleanup() deletes it.
         */
        TempDirInstanceManager(
            boost::asio::strand<boost::asio::any_io_executor> strand,
            std::filesystem::path tempRootDir,
            std::chrono::hours deadInstanceRetentionHours = std::chrono::hours{24}
        );

        ~TempDirInstanceManager() = default;

        TempDirInstanceManager(TempDirInstanceManager const&) = delete;
        TempDirInstanceManager& operator=(TempDirInstanceManager const&) = delete;
        TempDirInstanceManager(TempDirInstanceManager&&) = delete;
        TempDirInstanceManager& operator=(TempDirInstanceManager&&) = delete;

        /**
         * @brief Runs manualCleanup() once on the strand, for the instance directories that earlier runs left.
         *
         * Called once at start, so the scan runs before the frontend's first request.
         */
        void scheduleStartupCleanup();

        // ---- C++ API --------------------------------------------------------

        /**
         * @brief Create a new TemporaryDirectoryInstance and return a raw pointer to it.
         *
         * Ownership remains with the manager.  Must be called from the strand.
         *
         * @return Pointer to the new instance, or nullptr on failure.
         */
        TemporaryDirectoryInstance* createInstance();

        /**
         * @brief Destroy the instance with the given ID.
         *
         * If the instance does not exist this is a no-op.
         *
         * @param instanceId ID returned by TemporaryDirectoryInstance::instanceId().
         */
        void destroyInstance(std::string const& instanceId);

        /**
         * @brief Removes @p path, content of the instance @p instanceId that the user decided not to keep. End its
         *        watch first, or the removal is reported as a change. A link is removed itself, never followed. When nothing else is left in the instance directory, the instance is destroyed
         *        and, unlike destroyInstance(), its directory is removed as well (bookkeeping included, after its
         *        lock is released), so no orphan stays behind.
         *
         * @return An error when the instance does not exist, @p path is no content of it (see
         *         InstanceManagerDetail::removableContentPath) or cannot be removed.
         */
        std::expected<DiscardOutcome, std::string>
        discardContent(std::string const& instanceId, std::filesystem::path const& path);

        /**
         * @brief Ends the instance @p instanceId and removes its directory with everything in it, for an instance
         *        whose content is of no use, like the remains of a failed download. Nothing happens when there is none.
         */
        void removeInstance(std::string const& instanceId);

        /**
         * @brief How many instance directories below the root no live process holds.
         */
        std::size_t countOrphaned() const;

        /**
         * @brief Removes every instance directory below the root that no live process holds, regardless of the
         *        retention period.
         * @return How many were removed.
         */
        int removeOrphaned();

        /**
         * @brief The instance directories below the root that no live process holds, with what they hold and where
         *        it came from.
         */
        std::vector<SharedData::OrphanedCopy> listOrphaned() const;

        /**
         * @brief Removes the directory of the orphaned instance @p instanceId with everything in it.
         * @return An error when it is no orphaned instance below the root or cannot be removed.
         */
        std::expected<void, std::string> removeOrphanedInstance(Ids::InstanceId const& instanceId);

        /**
         * @brief Look up an instance by ID.
         *
         * @param instanceId ID to search for.
         * @return Pointer to the instance or nullptr when not found.
         */
        TemporaryDirectoryInstance* findInstance(std::string const& instanceId) const;

        /**
         * @brief Return an info snapshot for every currently managed instance.
         */
        std::vector<InstanceInfo> listInstances() const;

        /**
         * @brief Return the IDs of all currently managed instances.
         */
        std::vector<std::string> listInstanceIds() const;

        /**
         * @brief Scan @c tempRootDir for dead instance directories and clean up
         *        those that have exceeded the retention period.
         *
         * Directories that belong to live, managed instances are skipped.
         * Directories whose lock file is held by another (live) process are also
         * skipped.
         */
        void manualCleanup();

      private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        std::filesystem::path tempRootDir_;
        std::chrono::hours deadInstanceRetentionHours_;
        std::map<std::string, std::unique_ptr<TemporaryDirectoryInstance>> instances_;
    };
}
