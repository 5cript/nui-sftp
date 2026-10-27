#pragma once

#include <shared_data/directory_entry.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/archive_download_root.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_snapshot.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/sync_execution.hpp>
#include <nui-file-explorer/item.hpp>
#include <ids/ids.hpp>
#include <utility/lifetime_token.hpp>
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

    /**
     * @brief Lists the remote directory @p path.
     *
     * @param onComplete Receives the entries, or std::nullopt and the reason on failure.
     */
    void listDirectory(
        std::filesystem::path const& path,
        std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> entries, std::string const& info)>
            onComplete
    );

    /**
     * @brief Asks the backend for the remote home directory.
     *
     * @param onComplete Receives the home directory, or std::nullopt and the reason on failure.
     */
    void
    homeDirectory(std::function<void(std::optional<std::filesystem::path> const&, std::string const& info)> onComplete);

    /**
     * @brief Asks the backend for the XDG user directories configured on the server.
     *
     * @param home The remote home directory.
     * @param onComplete Receives pairs of XDG name (such as "DESKTOP") and path, or std::nullopt and the reason on
     * failure.
     */
    void userDirectories(
        std::filesystem::path const& home,
        std::function<void(
            std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> const&,
            std::string const& info
        )> onComplete
    );

    void
    createDirectory(Paths::RemotePath const& path, std::function<void(bool, std::string const& info)> onComplete);
    void createFile(Paths::RemotePath const& path, std::function<void(bool, std::string const& info)> onComplete);

    /**
     * @brief Opens a backend-resident SyncSession and queues both scans.  The scans
     *        build sorted ScanNode trees; the session diff-then-serves lazy subtrees.
     *
     * @param syncSessionId Pre-generated session id used for all follow-up RPCs.
     * @param remoteScanId  Operation id for the remote scan card.
     * @param localScanId   Operation id for the local scan card.
     */
    void openSyncSession(
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        Ids::SyncSessionId syncSessionId,
        Ids::OperationId remoteScanId,
        Ids::OperationId localScanId,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(bool success, std::string const& info)> onComplete
    );

    /**
     * @brief Runs (or re-runs) the diff on an open sync session and returns the
     *        summary.  @p onSummary receives a failed/empty summary with
     *        `cancelled=true` if the call fails.
     */
    void recomputeSyncDiff(
        Ids::SyncSessionId syncSessionId,
        SharedData::Sync::DiffOptions options,
        SharedData::Sync::LoadedParents loadedParents,
        std::function<void(SharedData::Sync::DiffSnapshot)> onSnapshot
    );

    /**
     * @brief Loads direct children of @p parentRelKey in @p section.  @p generation
     *        must match the most recent @ref recomputeSyncDiff reply, else the server
     *        rejects it as stale and the caller discards it.
     */
    void loadSyncDiffChildren(
        Ids::SyncSessionId syncSessionId,
        SharedData::Sync::DiffSection section,
        std::string const& parentRelKey,
        std::uint64_t generation,
        std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    /**
     * @brief Has the backend plan and queue the selected rows of one section, see
     *        @ref SharedData::Sync::SyncExecutionRequest.
     */
    void executeSyncSelection(
        SharedData::Sync::SyncExecutionRequest request,
        std::function<void(SharedData::Sync::SyncExecutionResult)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    /** @brief Flips the backend cancel flag. The running walk exits at next checkpoint. */
    void cancelSyncDiff(Ids::SyncSessionId syncSessionId);

    /** @brief Releases backend-side memory for @p syncSessionId. */
    void closeSyncSession(Ids::SyncSessionId syncSessionId);

    /**
     * @brief Goes on with the transfer @p operationId a cancel stopped keeping its partial files, the backend queues it
     *        again under its id, see OperationQueue::continueKept in the backend.
     */
    void continueKept(
        Ids::OperationId const& operationId,
        std::function<void(bool success, std::string const& info)> onDone
    );

    /**
     * @brief Deletes the partial files the transfers @p operationIds kept when they were canceled.
     */
    void discardKept(
        std::vector<Ids::OperationId> const& operationIds,
        std::function<void(bool success, std::string const& info)> onDone
    );

    void addDownload(
        Paths::RemotePath const& source,
        Paths::LocalPath const& destination,
        std::uint64_t sizeBytes,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void addUpload(
        Paths::LocalPath const& source,
        Paths::RemotePath const& destination,
        std::uint64_t sizeBytes,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Bulk-add downloads from a pre-known entry list.  Performs a
     *        single RPC for the whole batch; the backend trusts file sizes
     *        and skips its per-file lstat.  Directory entries fall back to
     *        the existing Scan+Bulk flow on the backend.
     *
     * @param entries              Mixed file + directory entries. Files must
     *                             have sizeBytes populated; directories set
     *                             isDirectory = true (size ignored).
     * @param allowOverwrite       Applied to every entry.
     * @param insertRefresh        Applied to every entry.
     * @param mode                 Queue vs. priority-queue for the batch.
     * @param onBulkCreated        Called once with the list of per-entry
     *                             OperationIds (pre-assigned by the caller
     *                             so completion callbacks can be registered
     *                             before the RPC returns).
     */
    void addBulkDownload(
        std::vector<SharedData::BulkDownloadEntry> entries,
        std::vector<Ids::OperationId> operationIds,
        bool allowOverwrite,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(bool success, std::string const& info)> onBulkCreated
    );

    /**
     * @brief Archive-download: pack a list of remote files into a single local
     *        tar[.ext] archive. @p compressionCodec is the raw integer value of
     *        TarArchive::Compression (backend-side); the frontend stays codec
     *        agnostic and the archive-transfer dialog produces the mapping.
     *        @p compressionLevel is in the codec's own range and @p xzExtreme
     *        only applies to xz.
     */
    void addArchiveDownload(
        std::vector<SharedData::ArchiveDownloadRoot> roots,
        Paths::LocalPath const& localArchivePath,
        int compressionCodec,
        int compressionLevel,
        bool xzExtreme,
        bool mayOverwrite,
        SharedData::OperationMode mode,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
    );

    /**
     * @brief Archive-upload: pack a list of local files into a remote tar[.ext]
     *        archive, streamed straight over SFTP. Same argument semantics as
     *        addArchiveDownload with source/destination reversed.
     */
    void addArchiveUpload(
        std::vector<Paths::LocalPath> localPaths,
        Paths::RemotePath const& remoteArchivePath,
        int compressionCodec,
        int compressionLevel,
        bool xzExtreme,
        bool mayOverwrite,
        SharedData::OperationMode mode,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
    );

    /** @brief Upload analogue of addBulkDownload, see its docs. */
    void addBulkUpload(
        std::vector<SharedData::BulkUploadEntry> entries,
        std::vector<Ids::OperationId> operationIds,
        bool allowOverwrite,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(bool success, std::string const& info)> onBulkCreated
    );

    /**
     * @brief Bulk-delete: collapse N file deletions into a single bulk-delete
     *        card on the frontend.  One RPC, one OperationId for the
     *        aggregate file card; directory entries get their own
     *        Scan+Delete cards on the backend.
     */
    void addBulkDelete(
        std::vector<SharedData::BulkDeleteEntry> entries,
        Ids::OperationId bulkOperationId,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(bool success, std::string const& info)> onBulkCreated
    );

    /**
     * @brief Asks the server which of @p directories are not empty and hands the split to
     *        @p onNonEmptyDirectoriesFound: files and empty directories first, non-empty directories second.
     */
    void remove(
        std::vector<Paths::RemotePath> files,
        std::vector<Paths::RemotePath> directories,
        std::function<void(bool, std::string const& info)> onComplete,
        std::function<void(std::vector<Paths::RemotePath>, std::vector<Paths::RemotePath>)> onNonEmptyDirectoriesFound
    );

    /**
     * @brief Use remove first and if it calls onNonEmptyDirectoriesFound, call this.
     *
     * @param paths
     * @param recursive
     * @param onComplete
     */
    void removeOnQueueUnchecked(
        std::vector<Paths::RemotePath> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Renames on the server. Fails with "backend.rename.targetExists" when @p newPath exists and
     *        @p overwrite is false.
     */
    void rename(
        Paths::RemotePath const& oldPath,
        Paths::RemotePath const& newPath,
        bool overwrite,
        std::function<void(bool, std::string const& info)> onComplete
    );

    /**
     * @brief Renames on this computer, needs no connection. Fails with "backend.rename.targetExists" when
     *        @p newPath exists and @p overwrite is false.
     */
    static void renameLocal(
        Paths::LocalPath const& oldPath,
        Paths::LocalPath const& newPath,
        bool overwrite,
        std::function<void(bool, std::string const& info)> onComplete
    );

    void addRename(
        Paths::RemotePath const& sourcePath,
        Paths::RemotePath const& destinationPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void stat(
        std::filesystem::path const& path,
        std::function<
            void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&, std::string const& info)>
            onComplete
    );

    /**
     * @brief Asks the server in one request which of @p paths exist.
     *
     * @param paths       Remote paths to probe.
     * @param onComplete  Receives a vector<bool> aligned with @p paths.  On
     *                    error the vector is empty and @p info carries the
     *                    diagnostic.
     */
    void existsBatchRemote(
        std::vector<Paths::RemotePath> const& paths,
        std::function<void(std::vector<bool> exists, std::string const& info)> onComplete
    );

    /**
     * @brief Asks the backend in one request which of the local @p paths exist, needs no connection.
     *
     * @param onComplete Same as for @ref existsBatchRemote.
     */
    static void existsBatchLocal(
        std::vector<Paths::LocalPath> const& paths,
        std::function<void(std::vector<bool> exists, std::string const& info)> onComplete
    );

    void dispose(std::function<void()> onComplete);

  private:
    void lazyOpen(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> const& onOpen);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
    /** @brief Drops the reply of the channel creation when this engine is gone. */
    Utility::LifetimeToken lifetime_{};
};
