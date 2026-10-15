#pragma once

#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/rpc_helper.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Nui
{
    class Window;
    class RpcHub;
}

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
         * @brief What the reply of FileTracking::fileStates lists: every regular file below @p instanceDirectory as
         *        {path, size, mtime} (seconds) and every directory as {path, directory: true}, paths canonical and
         *        UTF-8. Symbolic links and the bookkeeping directory are left out.
         */
        nlohmann::json fileStatesOf(std::filesystem::path const& instanceDirectory);

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
     * @brief Owns and tracks all TemporaryDirectoryInstance objects for a session.
     *
     * Inherits RpcHelper::StrandRpc so that all RPC handlers it registers run on
     * the shared strand.  The manager also owns the dead-instance cleanup policy:
     * the retention scan runs once at start (see scheduleStartupCleanup), the
     * frontend's Clean Up removes every orphaned directory at once (forceCleanup).
     *
     * ### Registered RPC endpoints
     * All names are prefixed with @c "FileTracking::".
     *
     * | Name                    | Parameters                          | Returns                        |
     * |-------------------------|-------------------------------------|--------------------------------|
     * | createInstance          | —                                   | {success, instanceId, instanceDir} |
     * | destroyInstance         | instanceId: string                  | {success}                      |
     * | listInstances           | —                                   | {success, instances: [...]}    |
     * | getInstanceInfo         | instanceId: string                  | {success, instanceId, instanceDir} |
     * | fileStates              | instanceId: string                  | {success, files: [...]}, see InstanceManagerDetail::fileStatesOf |
     * | countOrphaned           | —                                   | {success, count: int}          |
     * | forceCleanup            | —                                   | {success, removed: int}        |
     *
     * ### Push events (instance → frontend)
     * Each instance emits @c "FileTracking::\<instanceId\>::onFileChanged"
     * (see TemporaryDirectoryInstance).
     */
    class TempDirInstanceManager : public RpcHelper::StrandRpc
    {
      public:
        /**
         * @brief Construct the manager.
         *
         * @param executor               Executor for the shared strand.
         * @param wnd                    Nui window forwarded to each instance.
         * @param hub                    RPC hub forwarded to each instance and used
         *                               for manager RPC registrations.
         * @param tempRootDir            Root directory under which instance
         *                               subdirectories are created.
         * @param deadInstanceRetentionHours
         *                               How long a dead instance directory is kept
         *                               before manualCleanup() deletes it.
         */
        TempDirInstanceManager(
            boost::asio::any_io_executor executor,
            Nui::Window& wnd,
            Nui::RpcHub& hub,
            std::filesystem::path tempRootDir,
            std::chrono::hours deadInstanceRetentionHours = std::chrono::hours{24}
        );

        ~TempDirInstanceManager() = default;

        TempDirInstanceManager(TempDirInstanceManager const&) = delete;
        TempDirInstanceManager& operator=(TempDirInstanceManager const&) = delete;
        TempDirInstanceManager(TempDirInstanceManager&&) = delete;
        TempDirInstanceManager& operator=(TempDirInstanceManager&&) = delete;

        /**
         * @brief Register all RPC handlers listed in the class documentation.
         *
         * Must be called once after construction, from the strand or before the
         * strand starts processing.
         */
        void registerRpc();

        /**
         * @brief Runs manualCleanup() once on the strand, for the instance directories that earlier runs left.
         *
         * Called once at start, after registerRpc(), so the scan runs before the frontend's first request.
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
         * @brief Remove the watches of the instance with the given ID, then destroy it.
         *
         * If the instance does not exist this is a no-op.
         *
         * @param instanceId ID returned by TemporaryDirectoryInstance::instanceId().
         */
        void destroyInstance(std::string const& instanceId);

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
        std::filesystem::path tempRootDir_;
        std::chrono::hours deadInstanceRetentionHours_;
        Nui::Window* wnd_;
        std::map<std::string, std::unique_ptr<TemporaryDirectoryInstance>> instances_;
        std::map<std::string, std::vector<InstanceWatch>> watches_;
    };
}
