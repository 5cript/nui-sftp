#include <backend/file_tracking/temp_dir_instance_manager.hpp>
#include <backend/file_tracking/instance_bookkeeping.hpp>
#include <backend/file_tracking/file_states.hpp>
#include <backend/file_tracking/change_filter.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>
#include <paths/native.hpp>

#include <boost/asio/post.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <log/log.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <string_view>
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
        std::vector<FileState>
        fileStatesOf(std::filesystem::path const& instanceDirectory, std::filesystem::path const& below)
        {
            std::vector<FileState> files;
            // Below a canonical path the walk spells everything canonically, like the change events.
            std::error_code canonicalError{};
            const auto root = std::filesystem::weakly_canonical(instanceDirectory, canonicalError);
            if (!below.empty())
            {
                const auto canonicalBelow = std::filesystem::weakly_canonical(below, canonicalError);
                if (!isAtOrBelow(root, canonicalBelow) || isBookkeepingPath(root, canonicalBelow))
                    return files;
                if (canonicalBelow.lexically_normal() != root.lexically_normal())
                    return itemStatesOf(canonicalBelow);
            }
            std::error_code walkError{};
            for (std::filesystem::recursive_directory_iterator iter{root, walkError}, end;
                 !walkError && iter != end;
                 iter.increment(walkError))
            {
                if (iter.depth() == 0 && iter->path().filename() == std::filesystem::path{bookkeepingDirectoryName})
                {
                    iter.disable_recursion_pending();
                    continue;
                }
                if (auto state = fileStateOf(*iter))
                    files.push_back(*std::move(state));
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
        boost::asio::strand<boost::asio::any_io_executor> strand,
        std::filesystem::path tempRootDir,
        std::chrono::hours deadInstanceRetentionHours
    )
        : strand_{std::move(strand)}
        , tempRootDir_{std::move(tempRootDir)}
        , deadInstanceRetentionHours_{deadInstanceRetentionHours}
        , instances_{}
    {}

    void TempDirInstanceManager::scheduleStartupCleanup()
    {
        boost::asio::post(
            strand_,
            [this]()
            {
                manualCleanup();
            }
        );
    }

    TemporaryDirectoryInstance* TempDirInstanceManager::createInstance()
    {
        try
        {
            auto inst = std::make_unique<TemporaryDirectoryInstance>(
                TemporaryDirectoryInstance::Config{.tempRootDir = tempRootDir_}, strand_
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
        instances_.erase(instanceId);
    }

    std::expected<DiscardOutcome, std::string>
    TempDirInstanceManager::discardContent(std::string const& instanceId, std::filesystem::path const& path)
    {
        auto* instance = findInstance(instanceId);
        if (!instance)
            return std::unexpected{fmt::format("Instance not found: {}", instanceId)};
        const auto content = InstanceManagerDetail::removableContentPath(instance->instanceDir(), path);
        if (!content)
            return std::unexpected{content.error()};

        Log::info("Discarding '{}' of instance '{}'", Paths::nativeToUtf8(*content), instanceId);
        std::error_code removeError;
        Utility::removeAllNoFollow(*content, removeError);
        if (removeError)
            return std::unexpected{fmt::format(
                "Cannot remove '{}': {}", Paths::nativeToUtf8(*content), Utility::errorMessage(removeError)
            )};

        if (InstanceManagerDetail::hasContent(instance->instanceDir()))
            return DiscardOutcome::InstanceKept;
        instance->cleanupNow();
        destroyInstance(instanceId);
        return DiscardOutcome::InstanceDestroyed;
    }

    void TempDirInstanceManager::removeInstance(std::string const& instanceId)
    {
        auto* instance = findInstance(instanceId);
        if (!instance)
            return;
        Log::info("Removing instance '{}' with its content", instanceId);
        instance->cleanupNow();
        destroyInstance(instanceId);
    }

    std::size_t TempDirInstanceManager::countOrphaned() const
    {
        return orphanedInstanceDirs(tempRootDir_, listInstanceIds()).size();
    }

    int TempDirInstanceManager::removeOrphaned()
    {
        return forceCleanOrphanedInstanceDirs(tempRootDir_, listInstanceIds());
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
