#pragma once

#include <shared_data/directory_entry.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <nui-file-explorer/item.hpp>
#include <ids/ids.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <vector>
#include <optional>

class SshTerminalEngine;

class FileEngine
{
  public:
    FileEngine(SshTerminalEngine* engine);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FileEngine);

    std::optional<Ids::ChannelId> release();

    void listDirectory(
        std::filesystem::path const& path,
        std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> const&, std::string const& info)>
            onComplete
    );

    void
    createDirectory(std::filesystem::path const& path, std::function<void(bool, std::string const& info)> onComplete);
    void createFile(std::filesystem::path const& path, std::function<void(bool, std::string const& info)> onComplete);

    /** @brief Queues both a remote and a local scan as priority operations for sync comparison.
     *         The pre-generated IDs are used by the backend; the frontend registers listeners
     *         for those IDs before calling this so that no progress events are missed.
     *
     * @param localPath     Local directory root.
     * @param remotePath    Remote directory root.
     * @param remoteScanId  Pre-generated operation ID for the remote scan.
     * @param localScanId   Pre-generated operation ID for the local scan.
     * @param onComplete    Called on success/failure with (success, info).
     */
    void addSyncScans(
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        Ids::OperationId remoteScanId,
        Ids::OperationId localScanId,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(bool success, std::string const& info)> onComplete
    );

    void addDownload(
        NuiFileExplorer::Item const& remotePath,
        NuiFileExplorer::Item const& localPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void addUpload(
        NuiFileExplorer::Item const& remotePath,
        NuiFileExplorer::Item const& localPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void remove(
        std::vector<NuiFileExplorer::Item> const& files,
        std::vector<NuiFileExplorer::Item> const& directories,
        std::function<void(bool, std::string const& info)> onComplete,
        std::function<void(
            std::vector<std::filesystem::path>, /* regular files & empty */
            std::vector<std::filesystem::path> /* non empties */
        )> onNonEmptyDirectoriesFound
    );

    /**
     * @brief Use remove first and if it calls onNonEmptyDirectoriesFound, call this.
     *
     * @param paths
     * @param recursive
     * @param onComplete
     */
    void removeOnQueueUnchecked(
        std::vector<std::filesystem::path> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void rename(
        std::filesystem::path const& oldPath,
        std::filesystem::path const& newPath,
        std::function<void(bool, std::string const& info)> onComplete
    );

    void addRename(
        std::filesystem::path const& sourcePath,
        std::filesystem::path const& destinationPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void stat(
        std::filesystem::path const& path,
        std::function<
            void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&, std::string const& info)>
            onComplete
    );

    void dispose(std::function<void()> onComplete);

  private:
    void lazyOpen(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> const& onOpen);
    void performDelete(
        std::vector<NuiFileExplorer::Item> files,
        std::vector<std::filesystem::path> directoriesEmpty,
        std::function<void(bool, std::string const& info)> onComplete
    );

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
