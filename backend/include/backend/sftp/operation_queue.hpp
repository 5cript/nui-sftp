#pragma once

#include <backend/sftp/all_operations.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sync/sync_session.hpp>
#include <persistence/state/state.hpp>
#include <ssh/sftp_session.hpp>
#include <nui/rpc.hpp>
#include <ids/ids.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/bulk_add_request.hpp>
#include <shared_data/directory_entry.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/scan_node.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <atomic>

class OperationQueue
    : public RpcHelper::StrandRpc
    , public std::enable_shared_from_this<OperationQueue>
{
  public:
    constexpr static std::chrono::seconds defaultFutureTimeout{5};

    using Error = SharedData::OperationErrorType;
    using OperationCompleted = SharedData::OperationCompleted;
    using CompletionReason = SharedData::OperationCompletionReason;

  public:
    OperationQueue(
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::SftpOptions sftpOpts,
        Ids::SessionId sessionId,
        int parallelism = 1
    );

    void cancelAll();
    void cancel(Ids::OperationId id);

    /**
     * @brief Move @p operationId within the regular `operations_` queue to
     *        the absolute index @p newIndex.  Dispatched through the strand
     *        so it cannot interleave with `work()`; the running op (deque
     *        front) is therefore safely repositioned.
     *
     *        No-ops silently when:
     *          - the queue is not paused (the frontend must pause first;
     *            this is the safety belt for races against unpause),
     *          - the id refers to a priority op (priority queue is not
     *            user-reorderable in this UI),
     *          - the id is no longer in the queue (op completed meanwhile).
     *
     *        On a successful move emits `onOperationsReordered` so the
     *        frontend can reflect the new order without optimistic updates.
     */
    void moveOperation(Ids::OperationId operationId, std::size_t newIndex);

    /**
     * @brief Returns true if it should be called without delay again.
     *
     * @return true
     * @return false
     */
    bool work();

    std::expected<void, Operation::Error> addDownloadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        bool createMissingDirectories,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    std::expected<void, Operation::Error> addUploadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& localPath,
        std::filesystem::path const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        bool createMissingDirectories,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Enqueue an archive-download operation: pack a set of remote files
     *        into a single tar[.gz|.bz2|.zst|.xz] file on the local side.
     *
     * @param sftp               SFTP session providing the strand + remote access.
     * @param operationId        Pre-generated id so the frontend can register
     *                           progress / completion callbacks before the RPC returns.
     * @param entries            Metadata for each remote file to pack. Sizes are
     *                           taken at face value (no per-entry lstat).
     * @param localArchivePath   Destination archive path on the local filesystem.
     * @param compression        Compression codec (or Auto to pick from extension).
     * @param compressionLevel   User-facing 1..9 level (remapped to codec-native ranges).
     * @param mayOverwrite       Allow replacing an existing local archive.
     * @param mode               Standard queued / priority placement.
     */
    std::expected<void, Operation::Error> addArchiveDownloadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::vector<SharedData::DirectoryEntry> entries,
        std::filesystem::path const& localArchivePath,
        TarArchive::Compression compression,
        int compressionLevel,
        bool mayOverwrite,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Dual of addArchiveDownloadOperation: pack local files into a
     *        tar[.ext] archive streamed straight into a remote SFTP file.
     *        Each localPath is lstat'd on the SFTP strand at operation start;
     *        non-regular files are silently skipped.
     */
    std::expected<void, Operation::Error> addArchiveUploadOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::vector<std::filesystem::path> localPaths,
        std::filesystem::path const& remoteArchivePath,
        TarArchive::Compression compression,
        int compressionLevel,
        bool mayOverwrite,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    std::expected<void, Operation::Error> addDeleteOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& remotePath,
        bool recursive,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Bulk-add download operations from a pre-known entry list.
     *        Files are taken at face value (no lstat — sizes come from the
     *        caller) and turned into individual DownloadOperation cards.
     *        Directory entries fall back to the existing Scan+BulkDownload
     *        flow. Everything happens inside a single strand dispatch so
     *        only one SSH-thread context switch is paid for the batch.
     *
     * @param sftp           SFTP session to use.
     * @param request        Full bulk request (file + directory entries).
     * @param operationIdFor Callback producing a fresh OperationId per entry
     *                       index (frontend reserves them so it can register
     *                       completion callbacks before the RPC returns).
     * @return Number of entries successfully enqueued.
     */
    std::size_t addBulkDownloadOperation(
        SecureShell::SftpSession& sftp,
        SharedData::BulkAddRequest const& request,
        std::function<Ids::OperationId(std::size_t)> operationIdFor,
        Ids::OperationId bulkCardId
    );

    /** @brief Upload analogue of addBulkDownloadOperation.  File entries skip
     *         the per-file local lstat + RPC round-trip; directories fall
     *         back to the existing LocalScan+BulkUpload flow.
     *
     *  @param bulkCardId  Separate id reserved by the frontend for the aggregate
     *                     BulkUpload card that covers all file entries. Must not
     *                     collide with any of the per-entry ids produced by
     *                     @p operationIdFor. */
    std::size_t addBulkUploadOperation(
        SecureShell::SftpSession& sftp,
        SharedData::BulkAddRequest const& request,
        std::function<Ids::OperationId(std::size_t)> operationIdFor,
        Ids::OperationId bulkCardId
    );

    /**
     * @brief Bulk-delete: collapse N file entries (and any directory entries)
     *        into a single delete operation card on the frontend.  File
     *        entries are pre-known so no SFTP stat is needed.  Directory
     *        entries get the existing Scan+Delete pair so descendants are
     *        recursively removed.
     *
     * @param sftp           SFTP session to use.
     * @param request        Bulk request — `dst` is unused for delete, only
     *                       `src` (target path) and `isDirectory` matter.
     * @param bulkOperationId  Single OperationId for the aggregate file-bulk
     *                         card.  Per-directory ids are generated locally.
     * @return Number of entries successfully enqueued.
     */
    std::size_t addBulkDeleteOperation(
        SecureShell::SftpSession& sftp,
        SharedData::BulkAddRequest const& request,
        Ids::OperationId bulkOperationId
    );

    std::expected<void, Operation::Error> addRenameOperation(
        SecureShell::SftpSession& sftp,
        Ids::OperationId operationId,
        std::filesystem::path const& sourcePath,
        std::filesystem::path const& destinationPath,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /** @brief Queues a remote scan and a local scan as priority operations for sync comparison.
     *         When each scan completes the results are sent to the frontend via onSyncScanResult.
     *
     * @param sftp          SFTP session to use for the remote scan.
     * @param remoteScanId  Pre-assigned operation ID for the remote scan.
     * @param localScanId   Pre-assigned operation ID for the local scan.
     * @param remotePath    Remote directory root to scan.
     * @param localPath     Local directory root to scan.
     */
    /**
     * @brief Opens a @ref SyncSession and kicks off both scans. The scans build
     *        ScanNode trees directly and feed them into the session on completion.
     *        Emits @c onSyncScanPhaseDone(sessionId, isLocal) for each side.
     */
    void addSyncScanOperation(
        SecureShell::SftpSession& sftp,
        Ids::SyncSessionId syncSessionId,
        Ids::OperationId remoteScanId,
        Ids::OperationId localScanId,
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden
    );

    /**
     * @brief Returns the session with the given id, or nullptr when it has been
     *        closed / is unknown.  Callers must post onto @ref SyncSession::strand()
     *        before touching mutable state.
     */
    std::shared_ptr<SyncSession> syncSession(Ids::SyncSessionId const& sessionId) const;

    /**
     * @brief Removes the session from the registry.  Flips its cancel flag first so
     *        any in-flight walk exits at its next checkpoint.  The destructor runs
     *        once the last @c shared_ptr reference (captured tasks + this map) drops.
     */
    void closeSyncSession(Ids::SyncSessionId const& sessionId);

    void registerRpc();

    bool paused() const;
    void paused(bool pause);

    bool hasPriorityWork() const;

    /**
     * @brief Moves every still-tracked bulk-operation backup into @p
     *        registry, keyed by the bulk's OperationId.  Called by Session
     *        during teardown so a seamless reconnect can adopt them.
     *
     *        Covers both queued-not-started bulks and bulks already running
     *        — for the latter we re-issue the full original entry list and
     *        rely on the per-file tryContinue path to skip whole files
     *        already present (the in-progress file's bytes are not
     *        byte-checkpointed here).
     */
    void dumpBulkResumes(BulkResumeRegistry& registry);

    /**
     * @brief Re-enqueues the bulk operation described by @p entry against
     *        this queue, reusing @p operationId as the bulk-card id so the
     *        frontend's existing card surface stays attached.  Called from
     *        Session::adoptBulkResumes after pulling @p entry out of the
     *        registry.
     */
    void adoptBulkResume(SecureShell::SftpSession& sftp, Ids::OperationId const& operationId, BulkResumeEntry entry);

  private:
    void completeOperation(OperationCompleted&& operationCompleted);
    void deepPause(bool pause);

    std::string rpcName(std::string_view event) const;

    auto makeScanProgressCallback(std::string_view eventName, Ids::OperationId operationId)
        -> std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>;

    auto makeBulkProgressCallback(std::string_view eventName, Ids::OperationId operationId)
        -> std::function<void(
            std::filesystem::path const&,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::uint64_t,
            std::make_signed_t<std::size_t>
        )>;

  private:
    bool workQueue(std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>& queue);

  private:
    Persistence::SftpOptions sftpOpts_{};
    Ids::SessionId sessionId_{};
    std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>> priorityOperations_{};
    std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>> operations_{};
    std::atomic_bool paused_{true};
    int parallelism_{1};
    // Keyed by operationId.value(). Called when a sync-only scan completes and
    // hands off its ScanNode tree. The scan is known to be build-tree mode so
    // @ref ScanOperation::ejectScanTree() has the payload.
    std::map<std::string, std::function<void(SharedData::Sync::ScanNode)>> syncScanCallbacks_{};

    // SyncSessions active on this channel.  Keyed by SyncSessionId.value() because
    // Ids::IdHash isn't specialized on the strongly-typed id; the existing
    // bulkResumeStash_ does the specialization — we stay string-keyed for simplicity.
    std::map<std::string, std::shared_ptr<SyncSession>> syncSessions_{};

    // Per-bulk backup snapshot, populated when a bulk-add path enqueues an
    // op and erased once the op completes (success or failure).  Keyed by
    // the bulk's OperationId.  Read by dumpBulkResumes() during teardown.
    std::unordered_map<Ids::OperationId, BulkResumeEntry, Ids::IdHash> bulkResumeStash_{};
};
