#pragma once

#include <backend/file_tracking/temp_dir_instancing.hpp>
#include <backend/rpc_helper.hpp>

#include <boost/asio/any_io_executor.hpp>

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

    // -------------------------------------------------------------------------

    /**
     * @brief Owns and tracks all TemporaryDirectoryInstance objects for a session.
     *
     * Inherits RpcHelper::StrandRpc so that all RPC handlers it registers run on
     * the shared strand.  The manager also owns the dead-instance cleanup policy
     * and exposes it both as a C++ API and as RPC endpoints.
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
     * | manualCleanup           | —                                   | {success}                      |
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
         * @brief Destroy and remove the instance with the given ID.
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
