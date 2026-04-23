#pragma once

#include <shared_data/directory_entry.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/bulk_add_request.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/enqueue_plan_entry.hpp>
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
        std::function<void(SharedData::Sync::DiffSummary)> onSummary
    );

    /**
     * @brief Loads direct children of @p parentRelKey in @p section.  @p generation
     *        must match the most recent @ref recomputeSyncDiff reply or the server
     *        rejects as stale — the caller then discards.
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
     * @brief Asks the backend to collapse the selected relKey SPARSE set in @p section
     *        into a minimal enqueue plan.  An entry X in @p selectedRelKeys means "X
     *        and every descendant selected"; the backend-resident minimizer handles
     *        bulk-dir / structural-dir rules.
     */
    void buildSyncEnqueuePlan(
        Ids::SyncSessionId syncSessionId,
        SharedData::Sync::DiffSection section,
        std::vector<std::string> selectedRelKeys,
        std::uint64_t generation,
        std::function<void(std::vector<SharedData::Sync::EnqueuePlanEntry>)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    /** @brief Flips the backend cancel flag. The running walk exits at next checkpoint. */
    void cancelSyncDiff(Ids::SyncSessionId syncSessionId);

    /** @brief Releases backend-side memory for @p syncSessionId. */
    void closeSyncSession(Ids::SyncSessionId syncSessionId);

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
        std::vector<SharedData::BulkAddEntry> entries,
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
     *        @p compressionLevel is the user-facing 1..9 knob (re-scaled per
     *        codec on the backend).
     */
    void addArchiveDownload(
        std::vector<SharedData::DirectoryEntry> entries,
        std::filesystem::path const& localArchivePath,
        int compressionCodec,
        int compressionLevel,
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
        std::vector<std::filesystem::path> localPaths,
        std::filesystem::path const& remoteArchivePath,
        int compressionCodec,
        int compressionLevel,
        bool mayOverwrite,
        SharedData::OperationMode mode,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated
    );

    /** @brief Upload analogue of addBulkDownload — see its docs. */
    void addBulkUpload(
        std::vector<SharedData::BulkAddEntry> entries,
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
     *        Scan+Delete cards on the backend.  `dst` and `sizeBytes` of
     *        each entry are ignored — only `src` (target path) and
     *        `isDirectory` matter.
     */
    void addBulkDelete(
        std::vector<SharedData::BulkAddEntry> entries,
        Ids::OperationId bulkOperationId,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(bool success, std::string const& info)> onBulkCreated
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

    /**
     * @brief Batched remote-existence probe.  Replaces N per-file `sftp::stat`
     *        round-trips with a single one — used to gate bulk uploads on
     *        per-file conflict prompts without the latency cost.
     *
     * @param paths       Remote paths to probe.
     * @param onComplete  Receives a vector<bool> aligned with @p paths.  On
     *                    error the vector is empty and @p info carries the
     *                    diagnostic.
     */
    void existsBatchRemote(
        std::vector<std::filesystem::path> const& paths,
        std::function<void(std::vector<bool> exists, std::string const& info)> onComplete
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
