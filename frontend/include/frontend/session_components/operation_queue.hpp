#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <frontend/resumable_op.hpp>

#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operations_reordered.hpp>
#include <shared_data/file_operations/bulk_transfer_request.hpp>
#include <shared_data/file_operations/archive_download_root.hpp>
#include <shared_data/is_paused.hpp>
#include <shared_data/error_or_success.hpp>
#include <shared_data/sync/diff_snapshot.hpp>

#include <frontend/components/svg/play.hpp>
#include <frontend/components/svg/pause.hpp>
#include <frontend/components/svg/download.hpp>
#include <frontend/components/svg/upload.hpp>
#include <frontend/components/svg/scan.hpp>
#include <frontend/components/svg/scan_animated.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <utility/lifetime_token.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <chrono>

class LocalSideModel;
class RemoteSideModel;
class LocalOperationRouter;

class OperationQueue
{
  public:
    constexpr static std::chrono::seconds autoRemoveTime{5};

    /**
     * @param persistenceSessionName The name of the session in the settings, which keeps queue options changed in the
     * queue. Nothing for direct connects, which are not in the settings.
     * @param queueOptions The resolved queue options of the session.
     */
    OperationQueue(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::optional<std::string> persistenceSessionName,
        Persistence::QueueOptions const& queueOptions,
        ConfirmDialog* confirmDialog,
        LocalSideModel* localModel,
        RemoteSideModel* remoteModel
    );

    ROAR_PIMPL_SPECIAL_FUNCTIONS_NO_MOVE(OperationQueue);

    void activate(std::shared_ptr<FileEngine> fileEngine, Ids::SessionId sessionId);

    /**
     * @brief The file engine of the session, empty before @ref activate.
     */
    std::shared_ptr<FileEngine> fileEngine() const;
    void deactivate();

    void enqueueDownload(
        Paths::RemotePath const& source,
        Paths::LocalPath const& destination,
        std::uint64_t sizeBytes,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueUpload(
        Paths::LocalPath const& source,
        Paths::RemotePath const& destination,
        std::uint64_t sizeBytes,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueRename(
        Paths::RemotePath const& oldPath,
        Paths::RemotePath const& newPath,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueDelete(
        std::vector<Paths::RemotePath> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    /**
     * @brief Bulk-add downloads from a pre-known entry list.  One RPC for the
     *        whole batch; backend skips the per-file lstat and amortizes the
     *        SSH thread-switch across all entries in a single strand dispatch.
     *
     * @param entries         Mixed file + directory entries (set isDirectory
     *                        + sizeBytes accordingly).
     * @param allowOverwrite  Applied to every entry.
     * @param insertRefresh   Applied to every entry.
     * @param mode            Queue vs. priority-queue for the batch.
     * @param onEachComplete  Called once per entry upon its completion; the
     *                        OperationId allows per-entry completion tracking.
     *                        May be empty if the caller only needs the bulk
     *                        acknowledgement.
     * @param onBulkAck       Called once when the backend acknowledges the
     *                        batch (before per-entry completions fire).
     * @param onEnqueued      Invoked synchronously with the pre-generated
     *                        per-entry OperationIds (same order as @p entries)
     *                        before the RPC dispatches.  Lets callers register
     *                        transfer-progress callbacks per entry.
     */
    void enqueueBulkDownload(
        std::vector<SharedData::BulkDownloadEntry> entries,
        bool allowOverwrite,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(Ids::OperationId const& opId, bool success)> onEachComplete,
        std::function<void(bool success, std::string const& info)> onBulkAck,
        std::function<void(std::vector<Ids::OperationId> const& opIds)> onEnqueued = {}
    );

    /**
     * @brief Enqueue an archive-download: pack a list of remote files into one
     *        local tar[.ext] archive. @p compressionCodec is the raw int value
     *        of TarArchive::Compression (None=1, Gzip=2, Bzip2=3, Zstd=4, Xz=5
     *        — see the archive_transfer_dialog.hpp local enum); the frontend
     *        stays agnostic of codec specifics. @p compressionLevel is in the
     *        codec's own range and @p xzExtreme only applies to xz.
     */
    void enqueueArchiveDownload(
        std::vector<SharedData::ArchiveDownloadRoot> roots,
        Paths::LocalPath const& localArchivePath,
        int compressionCodec,
        int compressionLevel,
        bool xzExtreme,
        bool mayOverwrite,
        SharedData::OperationMode mode,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onOperationCreated
    );

    /** @brief Upload analogue of enqueueArchiveDownload — see its docs. */
    void enqueueArchiveUpload(
        std::vector<Paths::LocalPath> localPaths,
        Paths::RemotePath const& remoteArchivePath,
        int compressionCodec,
        int compressionLevel,
        bool xzExtreme,
        bool mayOverwrite,
        SharedData::OperationMode mode,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onOperationCreated
    );

    /** @brief Upload analogue of enqueueBulkDownload — see its docs. */
    void enqueueBulkUpload(
        std::vector<SharedData::BulkUploadEntry> entries,
        bool allowOverwrite,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(Ids::OperationId const& opId, bool success)> onEachComplete,
        std::function<void(bool success, std::string const& info)> onBulkAck,
        std::function<void(std::vector<Ids::OperationId> const& opIds)> onEnqueued = {}
    );

    /** @brief Bulk delete: one bulk-delete card for all file entries plus
     *         standard Scan+Delete pairs for any directory entries.
     *  @param onEnqueued  Invoked synchronously with the aggregate file-bulk
     *                     OperationId before the RPC dispatches. */
    void enqueueBulkDelete(
        std::vector<SharedData::BulkDeleteEntry> entries,
        bool insertRefresh,
        SharedData::OperationMode mode,
        std::function<void(bool success)> onBulkComplete,
        std::function<void(bool success, std::string const& info)> onBulkAck,
        std::function<void(Ids::OperationId const& bulkOpId)> onEnqueued = {}
    );

    /**
     * @brief Queues the delete of local items with their content in the backend's local operation queue, as one
     *        operation whose card shows here and refreshes the local listing when it ends. Needs no connection; with
     *        one the delete waits while this queue is paused. @p onAck gets the error when the backend refused it,
     *        nothing when it was queued.
     */
    void enqueueLocalDelete(
        std::vector<Paths::LocalPath> const& targets,
        std::function<void(std::optional<std::string> const& error)> onAck
    );

    /**
     * @brief The queue belongs to a connected session and can take operations.
     */
    bool isActive() const;

    void addCompletionCallback(Ids::OperationId const& opId, std::function<void(bool success)> callback);

    /**
     * @brief Receives the entries a bulk operation could not transfer or delete with why: by source path and, for a
     *        prescanned bulk transfer, by entry index.
     */
    using FailedEntriesCallback = std::function<void(
        std::vector<std::pair<std::string, SharedData::OperationError>> const& failedEntries,
        std::vector<SharedData::IndexedFailure> const& indexedFailures
    )>;

    /** @brief Register a callback that receives the entries a bulk operation could not transfer or
     *         delete. Runs on completion, before the completion callback, because the final BulkProgress
     *         can arrive after the completion and is dropped then.
     */
    void addFailedEntriesCallback(Ids::OperationId const& opId, FailedEntriesCallback callback);
    /** @brief Register a callback that receives progress as a 0.0–1.0 fraction for an upload/download.
     *         Automatically removed when the operation completes.
     */
    void addTransferProgressCallback(Ids::OperationId const& opId, std::function<void(double fraction)> callback);
    /** @brief Register a callback that receives BulkProgress events for a bulk
     *         up/download or delete.  Keyed by the aggregate operation id: for
     *         bulk up/download this is entries[0]'s id (as surfaced via
     *         onEnqueued), for bulk delete it is the single aggregate id.
     *         Auto-erased when the bulk operation completes.
     */
    void addBulkProgressCallback(
        Ids::OperationId const& aggregateOpId,
        std::function<void(SharedData::BulkProgress const&)> callback
    );
    void unpause();
    /** @brief Observable pause state — observe() it to react to pause/unpause changes. */
    Nui::Observed<bool>& pausedState();

    /** @brief Drops every callback registered for @p opId, for an operation that was never queued. */
    void removeCallbacks(Ids::OperationId const& opId);

    /** @brief Shows a restore button in the queue header for a minimized SyncDialog.
     *         The button pulses once on call (and on subsequent calls) to catch the user's eye.
     *
     * @param onRestore Callback invoked when the user clicks the restore button.
     */
    void showMinimizedSync(std::function<void()> onRestore);

    /** @brief Hides the minimized-sync restore button (e.g. when the dialog is closed or reopened). */
    void hideMinimizedSync();

    /**
     * @brief Opens a backend SyncSession and kicks both scans.  The two scans build
     *        sorted ScanNode trees on the backend; no entry payloads cross RPC.
     *
     *        The frontend learns when each side finishes via the per-side phase-done
     *        callback (separate from scan-progress events).  A backend
     *        @c onSyncDiffProgress stream — registered here too — is routed to the
     *        provider-level progress callback so the Comparing phase can animate.
     *
     * @param syncSessionId  Pre-allocated session id; reused across later RPCs.
     */
    void openSyncSession(
        Ids::SyncSessionId syncSessionId,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(SharedData::ScanProgress const&)> onRemoteProgress,
        std::function<void(SharedData::ScanProgress const&)> onLocalProgress,
        std::function<void(/*isLocal=*/bool)> onScanPhaseDone,
        std::function<void(std::string const& reason)> onScanFailed,
        std::function<void(std::uint64_t compared)> onDiffProgress
    );

    /**
     * @brief Clears per-session routing state installed by @ref openSyncSession.
     *        Called when the provider tears down (dialog close or session drop).
     */
    void clearSyncSessionRouting(Ids::SyncSessionId syncSessionId);

    // Thin pass-throughs to @ref FileEngine for the sync-session RPC surface.
    // Keep them on @ref OperationQueue so @ref BackendSyncProvider doesn't need
    // direct FileEngine access — matches the rest of the file-op RPCs here.

    void recomputeSyncDiff(
        Ids::SyncSessionId syncSessionId,
        SharedData::Sync::DiffOptions options,
        SharedData::Sync::LoadedParents loadedParents,
        std::function<void(SharedData::Sync::DiffSnapshot)> onSnapshot
    );

    void loadSyncDiffChildren(
        Ids::SyncSessionId syncSessionId,
        SharedData::Sync::DiffSection section,
        std::string const& parentRelKey,
        std::uint64_t generation,
        std::function<void(std::vector<SharedData::Sync::DiffTreeNode>)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    void executeSyncSelection(
        SharedData::Sync::SyncExecutionRequest request,
        std::function<void(SharedData::Sync::SyncExecutionResult)> onResolved,
        std::function<void(std::string const&)> onRejected
    );

    void cancelSyncDiff(Ids::SyncSessionId syncSessionId);
    void closeSyncSession(Ids::SyncSessionId syncSessionId);

    /**
     * @brief Extracts a resumable descriptor for every currently-in-flight
     *        (not yet completed) card in both the priority and normal queues.
     *        Used by Session::captureSnapshot to carry interrupted transfers
     *        across a seamless reconnect; the replacement Session re-enqueues
     *        each returned ResumableOp with tryContinue=true.
     */
    std::vector<ResumableOp> snapshotInFlight();

    /**
     * @brief Re-enqueues a non-bulk interrupted operation captured via
     *        snapshotInFlight().  Used by Session::applySnapshot during a
     *        seamless reconnect for the four single-file kinds
     *        (Download/Upload/Delete/Rename).  Bulk kinds are no-ops here
     *        — they are adopted backend-side via adoptBulkResumes().
     *
     *        Whether a re-enqueued single transfer resumes a partially-
     *        transferred file or starts fresh is governed by the session
     *        level tryContinue setting consumed by the backend; both
     *        paths are idempotent against the live filesystem state.
     */
    void enqueueResumable(ResumableOp const& op);

    Nui::ElementRenderer operator()();

  private:
    friend class LocalOperationRouter;

    template <typename OperationCard>
    void cancelOperation(OperationCard const& operation);
    void cancelAll();
    void togglePause();
    void askBackendToCancelAll();
    void changeAutoClean(bool doClean);

    void onOperationAdded(SharedData::OperationAdded const& added);
    void onDownloadProgress(SharedData::TransferProgress const& progress);
    void onUploadProgress(SharedData::TransferProgress const& progress);
    void onArchiveDownloadProgress(SharedData::TransferProgress const& progress);
    void onArchiveUploadProgress(SharedData::TransferProgress const& progress);
    void onBulkDownloadProgress(SharedData::BulkProgress const& progress);
    void onBulkUploadProgress(SharedData::BulkProgress const& progress);
    void onDeleteProgress(SharedData::BulkDeleteProgress const& progress);
    void onScanProgress(SharedData::ScanProgress const& progress);
    void onOperationCompleted(Nui::val val);
    void onOperationCompleted(SharedData::OperationCompleted completed);
    void onIsPaused(SharedData::ErrorOrSuccess<SharedData::IsPaused> const& result);
    void onOperationsReordered(SharedData::OperationsReordered const& evt);
    void requestMoveOperation(Ids::OperationId const& opId, std::size_t newIndex);

    /**
     * @brief Build the live regular-queue list element, attaching delegated
     *        drag handlers (one set of dragstart/dragover/dragleave/drop on
     *        the list container) so per-card cost stays O(1).  Modeled on
     *        icon_flavor.cpp / flavor_implementation.cpp's drag delegation.
     */
    Nui::ElementRenderer makeRegularLiveList();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    /** @brief Drops RPC replies and dialog answers that arrive after the queue is gone. */
    Utility::LifetimeToken lifetime_{};
};