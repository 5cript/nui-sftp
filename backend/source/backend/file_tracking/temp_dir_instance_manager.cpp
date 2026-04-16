#include <backend/file_tracking/temp_dir_instance_manager.hpp>

#include <utility/path_utf.hpp>

#include <nlohmann/json.hpp>
#include <log/log.hpp>

#include <chrono>
#include <fstream>
#include <system_error>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief Scan @p tempRootDir for dead instance directories and handle them.
         *
         * For each subdirectory whose name is not in @p skipIds:
         *  - If its lock file is held by another live process: skip.
         *  - Otherwise (dead):
         *      - If no death timestamp exists, write one now.
         *      - If a death timestamp exists and @c now - deadAt >= retentionHours,
         *        delete the directory recursively.
         *
         * @param tempRootDir     Root directory to scan.
         * @param skipIds         Set of instance IDs whose directories must not be touched.
         * @param retentionHours  Minimum age a dead directory must reach before deletion.
         */
        /**
         * @brief Classify a single subdirectory as orphaned or not.
         *
         * A directory is considered orphaned if:
         *  - It is not in skipIds (not a live instance of this process), AND
         *  - Its lock file exists but is not held by any live process, OR its
         *    metadata already carries a @c deadAt timestamp.
         *
         * @param dirPath  Path to the candidate subdirectory.
         * @param skipIds  IDs of live instances that must not be touched.
         * @return true if the directory is orphaned, false otherwise.
         */
        bool isOrphanedDir(
            std::filesystem::path const& dirPath,
            std::vector<std::string> const& skipIds
        )
        {
            namespace fs = std::filesystem;

            auto const dirName = dirPath.filename().string();
            for (auto const& sid : skipIds)
                if (sid == dirName)
                    return false;

            auto const lockPath = dirPath / (dirName + ".lock");
            if (!fs::exists(lockPath))
                return false;

            if (InstanceLock::isLockedByAnother(lockPath))
                return false;

            // Lock is not held — check if deadAt is already stamped
            auto const metaPath = dirPath / "metadata.json";
            if (fs::exists(metaPath))
            {
                try
                {
                    std::ifstream metaIn{metaPath};
                    nlohmann::json meta;
                    metaIn >> meta;
                    if (meta.contains("deadAt") && !meta["deadAt"].is_null())
                        return true; // deadAt stamp present → orphaned
                }
                catch (...) {}
            }

            // No deadAt yet but lock is free → also orphaned (dead process, no stamp written)
            return true;
        }

        /**
         * @brief Count orphaned instance directories under @p tempRootDir.
         *
         * @param tempRootDir  Root directory to scan.
         * @param skipIds      IDs of live instances to skip.
         * @return Number of orphaned directories found.
         */
        int countOrphanedInstanceDirs(
            std::filesystem::path const& tempRootDir,
            std::vector<std::string> const& skipIds
        )
        {
            namespace fs = std::filesystem;

            if (!fs::exists(tempRootDir))
                return 0;

            int count = 0;
            std::error_code iterErr;
            for (auto const& entry : fs::directory_iterator(tempRootDir, iterErr))
            {
                if (!entry.is_directory())
                    continue;
                if (isOrphanedDir(entry.path(), skipIds))
                    ++count;
            }
            return count;
        }

        /**
         * @brief Remove all orphaned instance directories under @p tempRootDir
         *        immediately, regardless of any retention period.
         *
         * @param tempRootDir  Root directory to scan.
         * @param skipIds      IDs of live instances that must not be deleted.
         * @return Number of directories successfully removed.
         */
        int forceCleanOrphanedInstanceDirs(
            std::filesystem::path const& tempRootDir,
            std::vector<std::string> const& skipIds
        )
        {
            namespace fs = std::filesystem;

            if (!fs::exists(tempRootDir))
                return 0;

            int removed = 0;
            std::error_code iterErr;
            for (auto const& entry : fs::directory_iterator(tempRootDir, iterErr))
            {
                if (!entry.is_directory())
                    continue;
                auto const& dirPath = entry.path();
                if (!isOrphanedDir(dirPath, skipIds))
                    continue;

                Log::info("forceCleanOrphanedInstanceDirs: removing orphaned instance '{}'", dirPath.filename().string());
                std::error_code rmErr;
                fs::remove_all(dirPath, rmErr);
                if (rmErr)
                    Log::warn("forceCleanOrphanedInstanceDirs: failed to remove '{}': {}", dirPath.filename().string(), rmErr.message());
                else
                    ++removed;
            }
            return removed;
        }

        void scanAndCleanDeadInstanceDirs(
            std::filesystem::path const& tempRootDir,
            std::vector<std::string> const& skipIds,
            std::chrono::hours retentionHours
        )
        {
            namespace fs = std::filesystem;

            if (!fs::exists(tempRootDir))
                return;

            auto const now = std::chrono::system_clock::now();

            std::error_code iterErr;
            for (auto const& entry : fs::directory_iterator(tempRootDir, iterErr))
            {
                if (!entry.is_directory())
                    continue;

                auto const& dirPath = entry.path();
                auto const dirName = dirPath.filename().string();

                bool skip = false;
                for (auto const& sid : skipIds)
                {
                    if (sid == dirName)
                    {
                        skip = true;
                        break;
                    }
                }
                if (skip)
                    continue;

                auto const lockPath = dirPath / (dirName + ".lock");
                if (!fs::exists(lockPath))
                    continue;

                if (InstanceLock::isLockedByAnother(lockPath))
                    continue; // still alive, held by another process

                // ---- Dead instance ----
                auto const metaPath = dirPath / "metadata.json";

                auto writeDeadAt = [&]()
                {
                    try
                    {
                        nlohmann::json meta;
                        if (fs::exists(metaPath))
                        {
                            std::ifstream metaIn{metaPath};
                            metaIn >> meta;
                        }
                        if (!meta.contains("instanceId"))
                            meta["instanceId"] = dirName;
                        meta["deadAt"] =
                            std::chrono::system_clock::to_time_t(now);
                        std::ofstream metaOut{metaPath};
                        metaOut << meta.dump(4);
                    }
                    catch (std::exception const& exc)
                    {
                        Log::warn(
                            "scanAndCleanDeadInstanceDirs: could not write deadAt for '{}': {}",
                            dirName,
                            exc.what()
                        );
                    }
                };

                if (!fs::exists(metaPath))
                {
                    writeDeadAt();
                    continue;
                }

                try
                {
                    std::ifstream metaIn{metaPath};
                    nlohmann::json meta;
                    metaIn >> meta;

                    if (meta.contains("deadAt") && !meta["deadAt"].is_null())
                    {
                        auto deadAtTs = meta["deadAt"].get<std::int64_t>();
                        auto deadAt = std::chrono::system_clock::from_time_t(
                            static_cast<std::time_t>(deadAtTs)
                        );
                        auto elapsed =
                            std::chrono::duration_cast<std::chrono::hours>(now - deadAt);

                        if (elapsed >= retentionHours)
                        {
                            Log::info(
                                "Removing dead instance '{}' (dead for {} hours)",
                                dirName,
                                elapsed.count()
                            );
                            std::error_code rmErr;
                            fs::remove_all(dirPath, rmErr);
                            if (rmErr)
                            {
                                Log::warn(
                                    "Failed to remove dead instance '{}': {}",
                                    dirName,
                                    rmErr.message()
                                );
                            }
                        }
                        // else: dead but within retention window, leave it
                    }
                    else
                    {
                        writeDeadAt();
                    }
                }
                catch (std::exception const& exc)
                {
                    Log::warn(
                        "scanAndCleanDeadInstanceDirs: error reading metadata for '{}': {}",
                        dirName,
                        exc.what()
                    );
                    writeDeadAt();
                }
            }
        }
    } // namespace

    // -------------------------------------------------------------------------
    // TempDirInstanceManager
    // -------------------------------------------------------------------------
    TempDirInstanceManager::TempDirInstanceManager(
        boost::asio::any_io_executor executor,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        std::filesystem::path tempRootDir,
        std::chrono::hours deadInstanceRetentionHours
    )
        : StrandRpc{std::move(executor), wnd, hub}
        , tempRootDir_{std::move(tempRootDir)}
        , deadInstanceRetentionHours_{deadInstanceRetentionHours}
        , wnd_{&wnd}
        , instances_{}
    {}

    void TempDirInstanceManager::registerRpc()
    {
        registerOnStrand(
            "FileTracking::createInstance",
            [this](RpcHelper::RpcOnce&& reply)
            {
                auto* inst = createInstance();
                if (!inst)
                {
                    reply.error("Failed to create instance");
                    return;
                }
                reply({
                    {"success", true},
                    {"instanceId", inst->instanceId()},
                    {"instanceDir", inst->instanceDir().string()},
                });
            }
        );

        registerOnStrand(
            "FileTracking::destroyInstance",
            [this](RpcHelper::RpcOnce&& reply, std::string instanceId)
            {
                auto iter = instances_.find(instanceId);
                if (iter == instances_.end())
                {
                    reply.error("Instance not found: " + instanceId);
                    return;
                }
                watches_.erase(instanceId);
                instances_.erase(iter);
                reply({{"success", true}});
            }
        );

        registerOnStrand(
            "FileTracking::addWatch",
            [this](RpcHelper::RpcOnce&& reply, std::string instanceId, std::string path, bool recursive)
            {
                auto* inst = findInstance(instanceId);
                if (!inst)
                {
                    reply.error("Instance not found: " + instanceId);
                    return;
                }
                auto watch = inst->addWatch(Utility::pathFromUtf8(path), recursive);
                if (!watch)
                {
                    reply.error("Failed to add watch on path: " + path);
                    return;
                }
                watches_[instanceId].push_back(std::move(*watch));
                reply({{"success", true}});
            }
        );

        registerOnStrand(
            "FileTracking::listInstances",
            [this](RpcHelper::RpcOnce&& reply)
            {
                auto arr = nlohmann::json::array();
                for (auto const& [instId, inst] : instances_)
                {
                    arr.push_back({
                        {"instanceId", instId},
                        {"instanceDir", inst->instanceDir().string()},
                    });
                }
                reply({{"success", true}, {"instances", std::move(arr)}});
            }
        );

        registerOnStrand(
            "FileTracking::getInstanceInfo",
            [this](RpcHelper::RpcOnce&& reply, std::string instanceId)
            {
                auto* inst = findInstance(instanceId);
                if (!inst)
                {
                    reply.error("Instance not found: " + instanceId);
                    return;
                }
                reply({
                    {"success", true},
                    {"instanceId", instanceId},
                    {"instanceDir", inst->instanceDir().string()},
                });
            }
        );

        registerOnStrand(
            "FileTracking::manualCleanup",
            [this](RpcHelper::RpcOnce&& reply)
            {
                manualCleanup();
                reply({{"success", true}});
            }
        );

        registerOnStrand(
            "FileTracking::countOrphaned",
            [this](RpcHelper::RpcOnce&& reply)
            {
                const int count = countOrphanedInstanceDirs(tempRootDir_, listInstanceIds());
                reply({{"success", true}, {"count", count}});
            }
        );

        registerOnStrand(
            "FileTracking::forceCleanup",
            [this](RpcHelper::RpcOnce&& reply)
            {
                const int removed = forceCleanOrphanedInstanceDirs(tempRootDir_, listInstanceIds());
                reply({{"success", true}, {"removed", removed}});
            }
        );
    }

    TemporaryDirectoryInstance* TempDirInstanceManager::createInstance()
    {
        try
        {
            auto inst = std::make_unique<TemporaryDirectoryInstance>(
                TemporaryDirectoryInstance::Config{.tempRootDir = tempRootDir_},
                *strand_,
                *wnd_,
                *hub_
            );
            auto* ptr = inst.get();
            instances_.emplace(inst->instanceId(), std::move(inst));
            return ptr;
        }
        catch (std::exception const& exc)
        {
            Log::error("TempDirInstanceManager::createInstance failed: {}", exc.what());
            return nullptr;
        }
    }

    void TempDirInstanceManager::destroyInstance(std::string const& instanceId)
    {
        instances_.erase(instanceId);
    }

    TemporaryDirectoryInstance*
    TempDirInstanceManager::findInstance(std::string const& instanceId) const
    {
        auto iter = instances_.find(instanceId);
        if (iter == instances_.end())
            return nullptr;
        return iter->second.get();
    }

    std::vector<InstanceInfo> TempDirInstanceManager::listInstances() const
    {
        std::vector<InstanceInfo> result;
        result.reserve(instances_.size());
        for (auto const& [instId, inst] : instances_)
            result.push_back({.instanceId = instId, .instanceDir = inst->instanceDir()});
        return result;
    }

    std::vector<std::string> TempDirInstanceManager::listInstanceIds() const
    {
        std::vector<std::string> result;
        result.reserve(instances_.size());
        for (auto const& [instId, inst] : instances_)
            result.push_back(instId);
        return result;
    }

    void TempDirInstanceManager::manualCleanup()
    {
        scanAndCleanDeadInstanceDirs(
            tempRootDir_,
            listInstanceIds(),
            deadInstanceRetentionHours_
        );
    }
}
