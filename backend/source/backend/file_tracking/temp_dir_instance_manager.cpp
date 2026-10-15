#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <paths/native.hpp>
#include <paths/local_path.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <log/log.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <system_error>
#include <vector>

namespace FileTracking
{
    namespace
    {
        /**
         * @brief The subdirectories of @p tempRootDir, the candidates for instance directories.
         */
        std::vector<std::filesystem::path> subdirectoriesOf(std::filesystem::path const& tempRootDir)
        {
            std::vector<std::filesystem::path> directories;
            std::error_code walkError;
            for (std::filesystem::directory_iterator iter{tempRootDir, walkError}, end; !walkError && iter != end;
                 iter.increment(walkError))
            {
                std::error_code typeError;
                if (iter->is_directory(typeError) && !iter->is_symlink(typeError))
                    directories.push_back(iter->path());
            }
            return directories;
        }

        /**
         * @brief Whether @p dirPath is the directory of an instance that no live process holds: it is not one of
         *        @p skipIds and has a lock file (in the bookkeeping directory or the legacy one) nobody holds.
         *
         * @param dirPath  Path to the candidate subdirectory.
         * @param skipIds  IDs of live instances that must not be touched.
         */
        bool isOrphanedDir(std::filesystem::path const& dirPath, std::vector<std::string> const& skipIds)
        {
            if (std::ranges::contains(skipIds, Paths::nativeToUtf8(dirPath.filename())))
                return false;
            const auto lock = existingLockFile(dirPath);
            return lock && !InstanceLock::isLockedByAnother(*lock);
        }

        /**
         * @brief The orphaned instance directories under @p tempRootDir, see isOrphanedDir.
         */
        std::vector<std::filesystem::path>
        orphanedInstanceDirs(std::filesystem::path const& tempRootDir, std::vector<std::string> const& skipIds)
        {
            auto directories = subdirectoriesOf(tempRootDir);
            std::erase_if(
                directories,
                [&skipIds](std::filesystem::path const& directory)
                {
                    return !isOrphanedDir(directory, skipIds);
                }
            );
            return directories;
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
            int removed = 0;
            for (auto const& dirPath : orphanedInstanceDirs(tempRootDir, skipIds))
            {
                Log::info(
                    "forceCleanOrphanedInstanceDirs: removing orphaned instance '{}'",
                    Paths::nativeToUtf8(dirPath.filename())
                );
                std::error_code rmErr;
                Utility::removeAllNoFollow(dirPath, rmErr);
                if (rmErr)
                    Log::warn(
                        "forceCleanOrphanedInstanceDirs: failed to remove '{}': {}",
                        Paths::nativeToUtf8(dirPath.filename()),
                        Utility::errorMessage(rmErr)
                    );
                else
                    ++removed;
            }
            return removed;
        }

        /**
         * @brief The death timestamp of an orphaned instance, nothing when it has none or its metadata is unreadable.
         */
        std::optional<std::chrono::system_clock::time_point> deadAtOf(std::optional<nlohmann::json> const& meta)
        {
            if (!meta || !meta->contains("deadAt") || !(*meta)["deadAt"].is_number_integer())
                return std::nullopt;
            return std::chrono::system_clock::from_time_t(
                static_cast<std::time_t>((*meta)["deadAt"].get<std::int64_t>())
            );
        }

        /**
         * @brief Scan @p tempRootDir for dead instance directories and handle them.
         *
         * For each orphaned directory (see isOrphanedDir):
         *  - If no death timestamp exists, write one now.
         *  - If a death timestamp exists and @c now - deadAt >= retentionHours,
         *    delete the directory recursively.
         *
         * @param tempRootDir     Root directory to scan.
         * @param skipIds         Set of instance IDs whose directories must not be touched.
         * @param retentionHours  Minimum age a dead directory must reach before deletion.
         */
        void scanAndCleanDeadInstanceDirs(
            std::filesystem::path const& tempRootDir,
            std::vector<std::string> const& skipIds,
            std::chrono::hours retentionHours
        )
        {
            const auto now = std::chrono::system_clock::now();
            for (auto const& dirPath : orphanedInstanceDirs(tempRootDir, skipIds))
            {
                const auto dirName = Paths::nativeToUtf8(dirPath.filename());
                const auto meta = readMetadata(dirPath);
                if (!meta)
                    Log::warn("scanAndCleanDeadInstanceDirs: error reading metadata for '{}': {}", dirName, meta.error());

                const auto deadAt = deadAtOf(meta.value_or(std::nullopt));
                if (!deadAt)
                {
                    auto stamped = meta.value_or(std::nullopt).value_or(nlohmann::json::object());
                    if (!stamped.is_object())
                        stamped = nlohmann::json::object();
                    if (!stamped.contains("instanceId"))
                        stamped["instanceId"] = dirName;
                    stamped["deadAt"] = std::chrono::system_clock::to_time_t(now);
                    if (const auto written = writeMetadata(dirPath, stamped); !written)
                        Log::warn(
                            "scanAndCleanDeadInstanceDirs: could not write deadAt for '{}': {}", dirName, written.error()
                        );
                    continue;
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::hours>(now - *deadAt);
                if (elapsed < retentionHours)
                    continue;

                Log::info("Removing dead instance '{}' (dead for {} hours)", dirName, elapsed.count());
                std::error_code rmErr;
                Utility::removeAllNoFollow(dirPath, rmErr);
                if (rmErr)
                    Log::warn("Failed to remove dead instance '{}': {}", dirName, Utility::errorMessage(rmErr));
            }
        }
    } // namespace

    namespace InstanceManagerDetail
    {
        nlohmann::json fileStatesOf(std::filesystem::path const& instanceDirectory)
        {
            nlohmann::json files = nlohmann::json::array();
            std::error_code walkError{};
            for (std::filesystem::recursive_directory_iterator iter{instanceDirectory, walkError}, end;
                 !walkError && iter != end;
                 iter.increment(walkError))
            {
                std::error_code entryError{};
                if (iter->is_symlink(entryError))
                    continue;
                if (iter.depth() == 0 && iter->path().filename() == std::filesystem::path{bookkeepingDirectoryName})
                {
                    iter.disable_recursion_pending();
                    continue;
                }
                // Canonical like the watched directory, so the paths match the change events.
                const auto path = Paths::nativeToUtf8(std::filesystem::weakly_canonical(iter->path(), entryError));
                if (iter->is_directory(entryError))
                {
                    files.push_back({{"path", path}, {"directory", true}});
                    continue;
                }
                if (!iter->is_regular_file(entryError))
                    continue;
                const auto size = iter->file_size(entryError);
                const auto modified = iter->last_write_time(entryError);
                if (entryError)
                    continue;
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::file_clock::to_sys(modified).time_since_epoch()
                );
                files.push_back({
                    {"path", path},
                    {"size", size},
                    {"mtime", seconds.count()},
                });
            }
            if (walkError)
                Log::warn(
                    "FileTracking::fileStates: walking '{}' failed: {}",
                    Paths::nativeToUtf8(instanceDirectory),
                    Utility::errorMessage(walkError)
                );
            return files;
        }

        std::expected<std::filesystem::path, std::string>
        removableContentPath(std::filesystem::path const& instanceDirectory, std::filesystem::path const& path)
        {
            const auto refused = [&path](std::string_view reason)
            {
                return std::unexpected{fmt::format("Cannot discard '{}': {}", Paths::nativeToUtf8(path), reason)};
            };
            const auto name = path.filename();
            if (name.empty() || name == "." || name == "..")
                return refused("it names no file");

            std::error_code canonicalError;
            const auto directory = std::filesystem::weakly_canonical(instanceDirectory, canonicalError);
            if (canonicalError)
                return refused(Utility::errorMessage(canonicalError));
            const auto parent = std::filesystem::weakly_canonical(path.parent_path(), canonicalError);
            if (canonicalError)
                return refused(Utility::errorMessage(canonicalError));

            const auto content = parent / name;
            const auto relative = content.lexically_relative(directory);
            if (relative.empty() || relative == "." || *relative.begin() == "..")
                return refused("it is not in the instance directory");
            if (*relative.begin() == std::filesystem::path{bookkeepingDirectoryName})
                return refused("it belongs to the bookkeeping of the instance");
            return content;
        }

        bool hasContent(std::filesystem::path const& instanceDirectory)
        {
            std::error_code walkError;
            std::filesystem::directory_iterator iter{instanceDirectory, walkError};
            if (walkError)
                return true;
            return std::any_of(
                iter,
                std::filesystem::directory_iterator{},
                [](std::filesystem::directory_entry const& entry)
                {
                    return entry.path().filename() != std::filesystem::path{bookkeepingDirectoryName};
                }
            );
        }
    }

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
                    {"instanceDir", Paths::nativeToUtf8(inst->instanceDir())},
                });
            }
        );

        registerOnStrand(
            "FileTracking::destroyInstance",
            [this](RpcHelper::RpcOnce&& reply, std::string instanceId)
            {
                if (!findInstance(instanceId))
                {
                    reply.error(fmt::format("Instance not found: {}", instanceId));
                    return;
                }
                destroyInstance(instanceId);
                reply({{"success", true}});
            }
        );

        registerOnStrand(
            "FileTracking::addWatch",
            [this](
                RpcHelper::RpcOnce&& reply,
                std::string instanceId,
                std::string path,
                bool recursive,
                std::string downloadTempSuffix
            )
            {
                auto* inst = findInstance(instanceId);
                if (!inst)
                {
                    reply.error(fmt::format("Instance not found: {}", instanceId));
                    return;
                }
                const auto watchedPath = Paths::LocalPath::fromUtf8(path);
                if (!watchedPath)
                {
                    reply.error(fmt::format("Cannot watch '{}': {}", path, watchedPath.error().toString()));
                    return;
                }
                auto watch = inst->addWatch(watchedPath->native(), recursive, std::move(downloadTempSuffix));
                if (!watch)
                {
                    reply.error(fmt::format("Failed to add watch on path: {}", path));
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
                        {"instanceDir", Paths::nativeToUtf8(inst->instanceDir())},
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
                    reply.error(fmt::format("Instance not found: {}", instanceId));
                    return;
                }
                reply({
                    {"success", true},
                    {"instanceId", instanceId},
                    {"instanceDir", Paths::nativeToUtf8(inst->instanceDir())},
                });
            }
        );

        registerOnStrand(
            "FileTracking::fileStates",
            [this](RpcHelper::RpcOnce&& reply, std::string instanceId)
            {
                auto* inst = findInstance(instanceId);
                if (!inst)
                {
                    reply.error(fmt::format("Instance not found: {}", instanceId));
                    return;
                }
                reply({{"success", true}, {"files", InstanceManagerDetail::fileStatesOf(inst->instanceDir())}});
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
                const auto count = orphanedInstanceDirs(tempRootDir_, listInstanceIds()).size();
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
            Log::error("TempDirInstanceManager::createInstance failed: {}", Utility::errorMessage(exc));
            return nullptr;
        }
    }

    void TempDirInstanceManager::destroyInstance(std::string const& instanceId)
    {
        watches_.erase(instanceId);
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
