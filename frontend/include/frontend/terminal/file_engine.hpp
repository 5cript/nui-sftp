#pragma once

#include <shared_data/directory_entry.hpp>
#include <nui-file-explorer/item.hpp>
#include <ids/ids.hpp>

#include <filesystem>
#include <vector>
#include <optional>

class FileEngine
{
  public:
    virtual ~FileEngine() = default;
    FileEngine() = default;
    FileEngine(const FileEngine& other) = default;
    FileEngine(FileEngine&& other) noexcept = default;
    FileEngine& operator=(const FileEngine& other) = default;
    FileEngine& operator=(FileEngine&& other) noexcept = default;

    virtual void listDirectory(
        std::filesystem::path const& path,
        std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> const&)> onComplete
    ) = 0;

    virtual void createDirectory(std::filesystem::path const& path, std::function<void(bool)> onComplete) = 0;
    virtual void createFile(std::filesystem::path const& path, std::function<void(bool)> onComplete) = 0;

    virtual void addDownload(
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        std::function<void(std::optional<Ids::OperationId>)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh
    ) = 0;
    virtual void addUpload(
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        std::function<void(std::optional<Ids::OperationId>)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh
    ) = 0;

    virtual void remove(
        std::vector<NuiFileExplorer::Item> const& files,
        std::vector<NuiFileExplorer::Item> const& directories,
        std::function<void(bool)> onComplete,
        std::function<void(
            std::vector<std::filesystem::path>, /* regular files & empty */
            std::vector<std::filesystem::path> /* non empties */
        )> onNonEmptyDirectoriesFound
    ) = 0;

    /**
     * @brief Use remove first and if it calls onNonEmptyDirectoriesFound, call this.
     *
     * @param paths
     * @param recursive
     * @param onComplete
     */
    virtual void removeOnQueueUnchecked(
        std::vector<std::filesystem::path> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&)> onComplete
    ) = 0;

    virtual void rename(
        std::filesystem::path const& oldPath,
        std::filesystem::path const& newPath,
        std::function<void(bool)> onComplete
    ) = 0;

    virtual void stat(
        std::filesystem::path const& path,
        std::function<void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&)> onComplete
    ) = 0;

    virtual void dispose(std::function<void()> onComplete) = 0;
};