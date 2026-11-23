#pragma once

#include <backend/sftp/all_operations.hpp>
#include <backend/local_operation_queue.hpp>
#include <backend/sftp/bulk_resume_registry.hpp>
#include <backend/sftp/kept_transfer_registry.hpp>
#include <backend/sftp/remote_delete_guard.hpp>
#include <backend/sftp/unsafe_fast_mode.hpp>
#include <backend/sync/sync_session.hpp>
#include <log/toast.hpp>
#include <persistence/state/state.hpp>
#include <remote_io/remote_file_system.hpp>
#include <nui/rpc.hpp>
#include <ids/ids.hpp>
#include <backend/rpc_helper.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_mode.hpp>
#include <shared_data/file_operations/partial_files_on_cancel.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/archive_download_root.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/compared_state.hpp>
#include <shared_data/file_operations/transfer_progress.hpp>
#include <paths/remote_path.hpp>
#include <shared_data/directory_entry.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>
#include <shared_data/sync/scan_node.hpp>
#include <shared_data/sync/sync_execution.hpp>

#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
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
    /** @brief What the frontend hears of a running transfer: a single file's, or a bulk's or directory's. */
    using OperationProgress = std::variant<SharedData::TransferProgress, SharedData::BulkProgress>;
    using ProgressObserver = std::function<void(OperationProgress const&)>;

  public:
    OperationQueue(
        boost::asio::any_io_executor executor,
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
        Nui::Window& wnd,
        Nui::RpcHub& hub,
        Persistence::SftpOptions sftpOpts,
        Ids::SessionId sessionId,
        std::shared_ptr<KeptTransferRegistry> keptTransfers,
        int parallelism = 1
    );

    /**
     * @brief Runs at most @p maxParallelTransfers transfers at once from now on, as a file system of the session
     *        demands. Nothing keeps the configured parallelism.
     */
    void limitParallelism(std::optional<std::size_t> maxParallelTransfers);

    using PartialFilesOnCancel = SharedData::PartialFilesOnCancel;

    /**
     * @brief Cancels every queued operation, each completes as cancelled. Those in the batch out are cancelled once
     *        they are back, what is queued after this stays.
     */
    void cancelAll(PartialFilesOnCancel partialFiles = PartialFilesOnCancel::AsConfigured);

    /**
     * @brief Drops every queued operation without completing any, for a session that goes: a reconnect may adopt
     *        their backups, see dumpBulkResumes(), and the frontend keeps their cards until then. Waits for the batch
     *        out first, which blocks the strand.
     */
    void dropAll();

    /**
     * @brief Cancels the operation @p id, it completes as cancelled, once it is back when it is in the batch out. A
     *        transfer that keeps its partial files can continue later, see continueKept and discardKept.
     */
    void cancel(Ids::OperationId id, PartialFilesOnCancel partialFiles = PartialFilesOnCancel::AsConfigured);

    /**
     * @brief Goes on with the transfer @p id a cancel stopped with its partial files: queues it again under its id on
     *        @p fileSystem, a bulk without what it finished. It continues its partial files whatever tryContinue says.
     * @return Why it was not queued: nothing kept under @p id, or it cannot continue. What it kept stays then, for
     *         discardKept.
     */
    std::expected<void, std::string>
    continueKept(std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem, Ids::OperationId const& id);

    /**
     * @brief Deletes the partial files the transfer @p id kept when it was canceled, remote ones on @p fileSystem.
     *        Behind a cancel of @p id that still waits for the batch out, which stores what it kept only then. On
     *        the strand.
     */
    void discardKept(std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem, Ids::OperationId const& id);

    /**
     * @brief Forgets the transfer @p id that kept its partial files when it was canceled. The files stay, nothing
     *        continues or discards them under @p id anymore. Behind a cancel of @p id that still waits for the batch
     *        out, which stores what it kept only then.
     */
    void forgetKept(Ids::OperationId id);

    /**
     * @brief Cancels the upload @p id like cancel(), the temporary file it wrote to stays for a later upload or goes,
     *        see @p partial.
     */
    void cancelUpload(Ids::OperationId id, UploadOperation::PartialFile partial);

    /**
     * @brief Removes the temporary file an upload to @p remotePath left on @p fileSystem, a cancelled one that kept it,
     *        and calls @p onDone on the strand then. Nothing is removed when there is none.
     */
    void discardPartialUpload(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Paths::RemotePath const& remotePath,
        std::function<void()> onDone
    );

    /**
     * @brief Move @p operationId within the regular `operations_` queue to
     *        the absolute index @p newIndex.  Dispatched through the strand
     *        so it cannot interleave with `work()`; the running op (deque
     *        front) is therefore safely repositioned.
     *
     *        No-ops silently when:
     *          - the queue is not paused when asked (the frontend must pause
     *            first; this is the safety belt for races against unpause),
     *          - the id refers to a priority op (priority queue is not
     *            user-reorderable in this UI),
     *          - the id is no longer in the queue (op completed meanwhile).
     *
     *        On a successful move emits `onOperationsReordered` so the
     *        frontend can reflect the new order without optimistic updates.
     */
    void moveOperation(Ids::OperationId operationId, std::size_t newIndex);

    /**
     * @brief Works the operations at the head of the queue once. Those that need a processing thread work there as
     *        one batch while the strand stays free; the batch wakes the queue when it returned and wants more.
     *
     * @return true to be called again right away, false when a batch is out, every operation waits or nothing can
     *         run: then the queue calls its wake once there is work again.
     */
    bool work();

    /**
     * @brief Waits until the batch out on a processing thread returned and puts its operations back. Blocks the strand,
     *        for a teardown that needs every operation. On the strand.
     * @return Whether the queue wants to be worked again right away, false when no batch was out.
     */
    bool drainBatch();

    /**
     * @brief Sets what is called, from any thread, once the queue has work after work returned false: an operation
     *        was added, cancelled, moved, paused or resumed, or a transfer progressed or ended.
     */
    void setWake(std::function<void()> wake);

    std::expected<void, Operation::Error> addDownloadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        Paths::LocalPath const& localPath,
        Paths::RemotePath const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        bool createMissingDirectories,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Queues the upload of the file, link or directory @p localPath to @p remotePath.
     * @param sourceMayVanish A source that is gone by the time the upload starts completes it without a transfer, see
     *        UploadOperation::UploadOperationOptions::sourceMayVanish. One that is gone already is refused as
     *        FileNotFound, which is no error worth logging.
     * @param comparedDestination What a file or link must find at @p remotePath to replace it, see
     *        UploadOperation::UploadOperationOptions::comparedDestination. A directory ignores it.
     * @param onSent Hears of each regular file sent unchanged, the files below a directory too, see
     *        UploadOperation::UploadOperationOptions::onSent.
     * @param adoptPartialOf The destination of a cancelled upload of the file under its old name, whose temporary
     *        file this one continues, see UploadOperation::UploadOperationOptions::adoptPartialOf. A directory
     *        ignores it.
     * @param failureToast Whether the errors logged when the upload is refused or fails are shown as toasts, see
     *        UploadOperation::UploadOperationOptions::failureToast.
     */
    std::expected<void, Operation::Error> addUploadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        Paths::LocalPath const& localPath,
        Paths::RemotePath const& remotePath,
        bool allowOverwrite,
        bool isBigFile,
        bool insertRefresh,
        bool createMissingDirectories,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued,
        bool sourceMayVanish = false,
        std::optional<SharedData::ComparedState> comparedDestination = std::nullopt,
        UploadOperation::OnSent onSent = {},
        std::optional<Paths::RemotePath> adoptPartialOf = std::nullopt,
        Log::Toast failureToast = Log::Toast::Show
    );

    /**
     * @brief Enqueue an archive-download operation: pack a set of remote files
     *        into a single tar[.gz|.bz2|.zst|.xz] file on the local side.
     *
     * @param fileSystem         The file system providing the strand and remote access.
     * @param operationId        Pre-generated id so the frontend can register
     *                           progress / completion callbacks before the RPC returns.
     * @param entries            Metadata for each remote file to pack. Sizes are
     *                           taken at face value (no per-entry lstat).
     * @param localArchivePath   Destination archive path on the local filesystem.
     * @param compression        Compression codec (or Auto to pick from extension).
     * @param compressionOptions Codec tuning, see TarArchive::compressionOptionsForLevel.
     * @param mayOverwrite       Allow replacing an existing local archive.
     * @param mode               Standard queued / priority placement.
     */
    std::expected<void, Operation::Error> addArchiveDownloadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        std::vector<SharedData::ArchiveDownloadRoot> roots,
        Paths::LocalPath const& localArchivePath,
        TarArchive::Compression compression,
        TarArchive::CompressionOptions const& compressionOptions,
        bool mayOverwrite,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Dual of addArchiveDownloadOperation: pack local files into a
     *        tar[.ext] archive streamed straight into a remote file.
     *        Each localPath is lstat'd on the file system's strand at operation start;
     *        non-regular files are silently skipped.
     */
    std::expected<void, Operation::Error> addArchiveUploadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        std::vector<Paths::LocalPath> localPaths,
        Paths::RemotePath const& remoteArchivePath,
        TarArchive::Compression compression,
        TarArchive::CompressionOptions const& compressionOptions,
        bool mayOverwrite,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Queues the delete of @p remotePath, refused by a @ref remoteDeleteGuard for the root and for the home
     *        directory or any directory that contains it.
     */
    std::expected<void, Operation::Error> addDeleteOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        Paths::RemotePath const& remotePath,
        bool recursive,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief @ref addDeleteOperation checking with @p guard, so a caller that checked the target with it already
     *        costs no further round trip.
     */
    std::expected<void, Operation::Error> addDeleteOperation(
        RemoteDeleteGuard& guard,
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        Paths::RemotePath const& remotePath,
        bool recursive,
        bool insertRefresh,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Queues the entries of @p request: each directory with its content as a bulk of its own that scans it
     *        first, under the id @p operationIdFor gives its index; all files, and directories created without their
     *        content, as one bulk under @p bulkCardId. Sizes and times of files are taken as given, without an lstat.
     *        Each queued part keeps a resume backup under its id.
     *
     * @param fileSystem The file system to use.
     * @param request The entries; the card of the files works below its roots, unset the closest directories
     *        holding every source and every destination.
     * @param operationIdFor The id of the entry at an index, reserved by the frontend so it can register callbacks
     *        before the reply.
     * @param bulkCardId The id of the card of the files, reserved by the frontend apart from the entry ids.
     * @return Number of entries queued: the files and directories of the card and the directories queued on their
     *         own. A request without any entry still queues the card, which then completes at once.
     */
    std::size_t addBulkDownloadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        SharedData::BulkDownloadRequest request,
        std::function<Ids::OperationId(std::size_t)> const& operationIdFor,
        Ids::OperationId const& bulkCardId
    );

    /**
     * @brief Upload analogue of @ref addBulkDownloadOperation.
     * @param sourceMayVanish Each entry may be gone by the time it is uploaded, see addUploadOperation.
     * @param onSent Hears of each regular file sent unchanged, see addUploadOperation.
     * @param failureToast Whether the errors logged when an entry is refused or fails are shown as toasts, see
     *        addUploadOperation.
     */
    std::size_t addBulkUploadOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        SharedData::BulkUploadRequest request,
        std::function<Ids::OperationId(std::size_t)> const& operationIdFor,
        Ids::OperationId const& bulkCardId,
        bool sourceMayVanish = false,
        UploadOperation::OnSent const& onSent = {},
        Log::Toast failureToast = Log::Toast::Show
    );

    /**
     * @brief Queues the remote items of @p request as one delete operation with one card, labelled with the
     *        directory they lie in. Entries a @ref remoteDeleteGuard refuses, and directories that could not be
     *        looked up in time, are failed entries of that card.
     *
     * @param fileSystem The file system to use.
     * @param request The remote items; directories are scanned and deleted with their content, unless the request
     *        deletes listed content only.
     * @param bulkOperationId Operation id of the one card.
     * @return Number of entries queued for deletion; zero queues nothing, not even the card.
     */
    std::size_t addBulkDeleteOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        SharedData::BulkDeleteRequest const& request,
        Ids::OperationId bulkOperationId
    );

    /**
     * @brief @ref addBulkDeleteOperation checking with @p guard, so a caller that checked the targets with it
     *        already costs no further round trip.
     */
    std::size_t addBulkDeleteOperation(
        RemoteDeleteGuard& guard,
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        SharedData::BulkDeleteRequest const& request,
        Ids::OperationId bulkOperationId
    );

    /**
     * @brief Queues a resolved sync plan under the operation ids of @p request: files and directories without
     *        content of each direction as one bulk transfer below the sync roots, remote deletes as one operation
     *        and local deletes as one operation of @p localDeleteQueue, held while this queue is paused. Every path
     *        comes from @p plan, which resolved them below the sync roots.
     */
    SharedData::Sync::SyncExecutionResult executeSyncPlan(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        SyncSession const& session,
        SyncSession::ResolvedPlan plan,
        SharedData::Sync::SyncExecutionRequest const& request,
        LocalOperationQueue& localDeleteQueue
    );

    /**
     * @brief A guard for the remote deletes of one request, sharing the home directory this queue looked up.
     */
    RemoteDeleteGuard remoteDeleteGuard(std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem);

    std::expected<void, Operation::Error> addRenameOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId operationId,
        Paths::RemotePath const& sourcePath,
        Paths::RemotePath const& destinationPath,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Opens a @ref SyncSession and kicks off both scans as priority operations. The scans build
     *        ScanNode trees directly and feed them into the session on completion.
     *        Emits @c onSyncScanPhaseDone(sessionId, isLocal) for each side.
     *
     * @param fileSystem    The file system to use for the remote scan.
     * @param remoteScanId  Pre-assigned operation ID for the remote scan.
     * @param localScanId   Pre-assigned operation ID for the local scan.
     * @param remotePath    Remote directory root to scan.
     * @param localPath     Local directory root to scan.
     */
    void addSyncScanOperation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::SyncSessionId syncSessionId,
        Ids::OperationId remoteScanId,
        Ids::OperationId localScanId,
        Paths::RemotePath const& remotePath,
        Paths::LocalPath const& localPath,
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

    /**
     * @brief Whether files that open from now on are transferred in the unsafe fast mode, see UnsafeFastMode. Off for
     *        every new queue. Uploads of File Tracking always stay safe, they continue partial files across renames.
     */
    bool unsafeFast() const;
    void unsafeFast(bool enabled);

    /**
     * @brief Test hook: @p observer sees every completion right before it is sent to the frontend. The application
     *        never sets one.
     */
    void setCompletionObserver(std::function<void(OperationCompleted const&)> observer);

    /**
     * @brief Calls @p callback on the strand once the operation @p operationId completed, failed or was cancelled,
     *        after the frontend was told. Must be called from the strand, before the operation can complete.
     */
    void whenCompleted(Ids::OperationId const& operationId, std::function<void(OperationCompleted const&)> callback);

    /**
     * @brief Calls @p observer on the strand with each progress of the operation @p operationId the frontend hears of,
     *        until it completed. Must be called from the strand, before the operation starts.
     */
    void whenProgressing(Ids::OperationId const& operationId, ProgressObserver observer);

    /**
     * @brief Keeps no resume backup of the operation @p operationId, a reconnect does not queue it again. For an owner
     *        that decides anew what goes to the server. Must be called from the strand.
     */
    void discardResumeBackup(Ids::OperationId const& operationId);

    /**
     * @brief The owner of the operation @p operationId asks the user about destinations that changed since their
     *        compare, so such an end is no failure of the queue's: the operation completes as canceled with the error
     *        that tells the owner why, and a bulk leaves those entries out of its failed ones while its indexed
     *        failures keep them. Must be called from the strand.
     */
    void leaveChangedDestinationsToOwner(Ids::OperationId const& operationId);

    bool hasPriorityWork() const;

    /**
     * @brief Moves every resume backup into @p registry, keyed by the id of its card. Called by Session during
     *        teardown so a seamless reconnect can adopt them.
     *
     *        A bulk transfer that has its entries already leaves only the entries it has not finished, as a flat
     *        list below its roots, and records how many entries and bytes it finished, which the resumed card counts
     *        as done; a running file in it starts over unless its partial file can be continued. A bulk that did not
     *        start yet or is still scanning keeps its request. Waits for the batch out first, which blocks the strand.
     */
    void dumpBulkResumes(BulkResumeRegistry& registry);

    /**
     * @brief Re-enqueues the backup @p entry under @p operationId as the id of every part, so the frontend's card
     *        stays attached. Refuses a backup of more than one directory, or of a directory and other entries,
     *        which would complete the card more than once. Called from Session::adoptBulkResumes after pulling
     *        @p entry out of the registry.
     * @return Whether it was queued.
     */
    bool adoptBulkResume(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId const& operationId,
        BulkResumeEntry entry
    );

  private:
    void completeOperation(OperationCompleted&& operationCompleted);
    void deepPause(bool pause);

    /**
     * @brief The mode a transfer of this queue asks when it opens a file. For an upload that tells @p onSent of its
     *        files, File Tracking's, the safe mode.
     */
    UnsafeFastMode unsafeFastModeFor(UploadOperation::OnSent const& onSent = {}) const;

    std::string rpcName(std::string_view event) const;

    auto makeScanProgressCallback(std::string_view eventName, Ids::OperationId operationId)
        -> std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>;

    auto makeBulkProgressCallback(std::string_view eventName, Ids::OperationId operationId) -> BulkProgressCallback;

    /**
     * @brief Remembers whether the failure of the queued operation @p operationId is logged with a toast.
     */
    void rememberFailureToast(Ids::OperationId const& operationId, Log::Toast failureToast);

    /**
     * @brief Sends the delete progress of @p operationId (current item, items handled, items in all) to the frontend.
     */
    auto makeDeleteProgressCallback(Ids::OperationId operationId)
        -> std::function<void(std::string const&, std::uint64_t, std::uint64_t)>;

  private:
    using QueuedOperations = std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>;
    struct WorkResult;
    struct Batch;

    bool workQueue(QueuedOperations& queue);

    /**
     * @brief Works the first @p count operations of @p operations in order, only those whose usesStrand equals
     *        @p onlyUsesStrand when set, and stops after the first that completed or failed. One that does not use
     *        the strand waits with a timeout for its processing thread, so that thread runs what is queued there first.
     */
    static std::vector<WorkResult>
    workHead(QueuedOperations& operations, std::size_t count, std::optional<bool> onlyUsesStrand);

    /**
     * @brief Hands the first @p count operations of @p queue to @p strand as one batch, see Batch.
     */
    void
    startBatch(QueuedOperations& queue, std::size_t count, RemoteIo::ProcessingStrand& strand, std::size_t strandIndex);

    /**
     * @brief Puts the operations of @p batch back at the head of their queue and applies what they did, unless it is
     *        not the batch out any more. Then runs the requests that waited for it.
     * @return Whether the queue wants to be worked again right away, which also wakes it.
     */
    bool finishBatch(std::shared_ptr<Batch> const& batch);

    /**
     * @brief Applies @p results of the operations at the head of @p queue: one that completed or failed leaves the
     *        queue after @p heldProgress went out, and its owner hears of it.
     * @return Whether the queue wants to be worked again right away.
     */
    bool applyResults(
        QueuedOperations& queue,
        std::vector<WorkResult> const& results,
        std::vector<std::function<void()>>& heldProgress
    );

    /**
     * @brief Runs @p request with the queue on the strand, callable from any thread. Nothing runs once the queue is
     *        gone.
     */
    template <typename FunctionT>
    void onStrand(FunctionT&& request);

    /**
     * @brief Runs @p request once no batch of this queue works on a processing thread, at once when none does. A
     *        request on the whole queue, a move or a pause, goes through here. It only runs while the queue exists, so
     *        it may refer to it. On the strand.
     */
    void afterBatch(std::function<void()> request);

    /**
     * @brief Runs @p request once the operation @p id is back from the batch out, at once when it is queued or gone. A
     *        request on one operation, a cancel, goes through here. It only runs while the queue exists. On the strand.
     */
    void onOperation(Ids::OperationId const& id, std::function<void()> request);

    /**
     * @brief The ids of the operations in the batch out, none while none is.
     */
    std::vector<Ids::OperationId> batchOperationIds() const;

    /**
     * @brief Cancels the queued operation @p id as cancel() does, its sync scan fails. On the strand.
     */
    void cancelQueued(Ids::OperationId const& id, PartialFilesOnCancel partialFiles);

    /**
     * @brief Moves the queued operation @p operationId to @p newIndex of the paused queue, see moveOperation(). On the
     *        strand.
     */
    void reorder(Ids::OperationId const& operationId, std::size_t newIndex);

    /**
     * @brief Tells the frontend whether the move of @p operationId to @p index was @p applied. Every move ends in one,
     *        the frontend's listener applies or discards its reorder by it.
     */
    void reportReorder(Ids::OperationId const& operationId, std::size_t index, bool applied);

    /**
     * @brief The queue an operation of @p mode goes to.
     */
    auto queueFor(SharedData::OperationMode mode)
        -> std::deque<std::pair<Ids::OperationId, std::unique_ptr<Operation>>>&;

    /**
     * @brief Shared part of the bulk transfer requests: each directory with content goes to @p queueDirectory as
     *        (index, entry, request options) and counts when it returns true, every other entry into one request that
     *        @p queueFiles queues as the card @p bulkCardId and sets the roots of. That request is the card's backup.
     *
     * @return Number of entries queued.
     */
    template <typename RequestT, typename QueueDirectoryT, typename QueueFilesT>
    std::size_t splitBulkRequest(
        RequestT request,
        Ids::OperationId const& bulkCardId,
        QueueDirectoryT&& queueDirectory,
        QueueFilesT&& queueFiles
    );

    /**
     * @brief The queued operation @p operationId, none when there is none or it is in the batch out.
     */
    Operation* findOperation(Ids::OperationId const& operationId);

    /**
     * @brief Reduces the backup @p entry of @p operation to what the operation has not finished yet and records
     *        what it left out in @p entry, which the resumed card counts as done.
     */
    static void leaveOutFinished(Operation const& operation, BulkResumeEntry& entry);

    /**
     * @brief Tells the sync session waiting for the scan @p id, if any, that it failed or was cancelled.
     * @param reason A localized message, see Utility::localizedMessage.
     */
    void failSyncScan(Ids::OperationId const& id, std::string const& reason);

    /**
     * @brief Drops the scan @p id of a sync whose other scan failed, without telling its session again.
     */
    void cancelSiblingScan(Ids::OperationId const& id);

    /**
     * @brief Takes the operation @p id out of the queue it waits in, nothing when no queue has it.
     */
    std::unique_ptr<Operation> takeOperation(Ids::OperationId const& id);

    /**
     * @brief Cancels @p operation, already taken out of its queue, and completes it as cancelled.
     */
    void completeCancelled(Ids::OperationId const& id, Operation& operation);

    /**
     * @brief Cancels @p operation for the user like completeCancelled, its partial files as @p partialFiles says.
     *        The kept ones are remembered for continueKept and discardKept.
     */
    void completeCancelledByUser(Ids::OperationId const& id, Operation& operation, PartialFilesOnCancel partialFiles);

    /**
     * @brief Deletes what the transfer @p id kept, see discardKept.
     */
    void discardKeptNow(std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem, Ids::OperationId const& id);

    /**
     * @brief Remembers the partial files the canceled @p operation kept, with how to continue it: what is left of its
     *        bulk backup, or the transfer of a single file.
     * @return Whether it kept them.
     */
    bool rememberKeptPartialFiles(Ids::OperationId const& id, Operation const& operation);

    /**
     * @brief Queues @p continuation under @p id with tryContinue on, whatever the options say.
     * @return Why it was not queued.
     */
    std::expected<void, std::string> queueContinuation(
        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem,
        Ids::OperationId const& id,
        KeptContinuation continuation
    );

    /**
     * @brief Hands the tree of a completed standalone scan to the sync session registered for its id.
     */
    void deliverSyncScan(Ids::OperationId const& id, Operation& operation);

    /**
     * @brief The callback of the scan of one side of a sync: hands the tree to @p session through @p setTree on the
     *        session's strand and tells the frontend the side is done, or cancels the other scan @p siblingScanId and
     *        reports the failure.
     */
    auto makeSyncScanCallback(
        Ids::SyncSessionId syncSessionId,
        std::weak_ptr<SyncSession> session,
        Ids::OperationId siblingScanId,
        bool isLocal,
        void (SyncSession::*setTree)(SharedData::Sync::ScanNode)
    ) -> std::function<void(std::expected<SharedData::Sync::ScanNode, std::string>)>;

  private:
    void wake() const;

    /**
     * @brief Wakes the queue's driver when the method it guards returns, whatever it changed.
     */
    struct WakeOnReturn
    {
        OperationQueue const& queue;
        ~WakeOnReturn()
        {
            queue.wake();
        }
    };

  private:
    std::function<void()> wake_{};
    Persistence::SftpOptions sftpOpts_{};
    Ids::SessionId sessionId_{};
    QueuedOperations priorityOperations_{};
    QueuedOperations operations_{};
    /** @brief The batch out on a processing thread, nothing while none is. */
    std::shared_ptr<Batch> batch_{};
    /** @brief What waits for batch_ to return, see afterBatch() and onOperation(). */
    std::vector<std::function<void()>> afterBatch_{};
    /** @brief work() was called while batch_ was out, so the queue is woken once it returned. */
    bool workAskedDuringBatch_{false};
    std::atomic_bool paused_{true};
    /** @brief Shared with the transfers, which read it on the processing thread when they open a file. */
    std::shared_ptr<std::atomic_bool> unsafeFast_{std::make_shared<std::atomic_bool>(false)};
    std::function<void(OperationCompleted const&)> completionObserver_{};
    std::unordered_map<Ids::OperationId, std::function<void(OperationCompleted const&)>, Ids::IdHash>
        completionCallbacks_{};
    /** @brief See whenProgressing(). */
    std::unordered_map<Ids::OperationId, ProgressObserver, Ids::IdHash> progressObservers_{};
    /** @brief See leaveChangedDestinationsToOwner(). */
    std::unordered_set<Ids::OperationId, Ids::IdHash> changedDestinationsLeftToOwner_{};
    /** @brief The operations whose failure is logged without a toast, see rememberFailureToast(). */
    std::unordered_set<Ids::OperationId, Ids::IdHash> failuresWithoutToast_{};
    // Transfer slots handed out per queue tick.
    int parallelism_{1};
    // Upper bound on the slots a single bulk transfer may take, below parallelism_.
    int bulkConcurrency_{1};
    // Keyed by operationId.value(). Called when a sync-only scan completes and
    // hands off its ScanNode tree. The scan is known to be build-tree mode so
    // @ref ScanOperation::ejectScanTree() has the payload.
    std::map<std::string, std::function<void(std::expected<SharedData::Sync::ScanNode, std::string>)>>
        syncScanCallbacks_{};

    // SyncSessions active on this channel, keyed by SyncSessionId.value().
    std::map<std::string, std::shared_ptr<SyncSession>> syncSessions_{};

    // Per-bulk backup snapshot, populated when a bulk-add path enqueues an
    // op and erased once the op completes (success or failure).  Keyed by
    // the bulk's OperationId.  Read by dumpBulkResumes() during teardown.
    std::unordered_map<Ids::OperationId, BulkResumeEntry, Ids::IdHash> bulkResumeStash_{};

    /** @brief Shared with the queues of the other sessions, see KeptTransferRegistry. */
    std::shared_ptr<KeptTransferRegistry> keptTransfers_;

    /** @brief The canonical home directory of the server, asked once for the delete guards. */
    std::optional<Paths::RemotePath> remoteHome_{};
};
