#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/terminal/file_engine.hpp>

#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/bulk_delete_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/sync_scan_result.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/is_paused.hpp>
#include <shared_data/error_or_success.hpp>

#include <frontend/components/progress_bar.hpp>
#include <frontend/components/svg/play.hpp>
#include <frontend/components/svg/pause.hpp>
#include <frontend/components/svg/download.hpp>
#include <frontend/components/svg/upload.hpp>
#include <frontend/components/svg/scan.hpp>
#include <frontend/components/svg/scan_animated.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <chrono>

class LocalSideModel;
class RemoteSideModel;

class OperationQueue
{
  public:
    constexpr static std::chrono::seconds autoRemoveTime{5};

    OperationQueue(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        std::string persistenceSessionName,
        ConfirmDialog* confirmDialog,
        LocalSideModel* localModel,
        RemoteSideModel* remoteModel
    );

    ROAR_PIMPL_SPECIAL_FUNCTIONS(OperationQueue);

    void activate(std::shared_ptr<FileEngine> fileEngine, Ids::SessionId sessionId);
    void deactivate();

    void enqueueDownload(
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueUpload(
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        bool allowOverwrite,
        bool insertRefresh,
        bool createMissingDirectories = false,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueRename(
        std::filesystem::path const& oldPath,
        std::filesystem::path const& newPath,
        std::function<void(std::optional<Ids::OperationId> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );
    void enqueueDelete(
        std::vector<std::filesystem::path> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete,
        SharedData::OperationMode mode = SharedData::OperationMode::Queued
    );

    void addCompletionCallback(Ids::OperationId const& opId, std::function<void(bool success)> callback);
    /** @brief Register a callback that receives progress as a 0.0–1.0 fraction for an upload/download.
     *         Automatically removed when the operation completes.
     */
    void addTransferProgressCallback(Ids::OperationId const& opId, std::function<void(double fraction)> callback);
    void unpause();
    /** @brief Observable pause state — observe() it to react to pause/unpause changes. */
    Nui::Observed<bool>& pausedState();

    /** @brief Enqueues a priority remote scan and a priority local scan for sync comparison.
     *         All callbacks are registered before the backend RPC is called to avoid missing progress.
     *
     * @param localPath        Local directory root to scan.
     * @param remotePath       Remote directory root to scan.
     * @param onRemoteProgress Called on each remote scan progress update.
     * @param onLocalProgress  Called on each local scan progress update.
     * @param onRemoteComplete Called with the remote scan result when it finishes.
     * @param onLocalComplete  Called with the local scan result when it finishes.
     */
    /** @brief Fires a remote-side createDirectory RPC.  Used by the sync dialog in
     *         non-recursive mode where transferring a missing directory must not
     *         turn into a recursive bulk upload that could clobber existing files.
     *         Fire-and-forget (no operation ID, no queue entry).
     */
    void createRemoteDirectory(
        std::filesystem::path const& path,
        std::function<void(bool success, std::string const& info)> onComplete
    );

    /** @brief Fires a local-side createDirectory RPC (mirror of createRemoteDirectory). */
    void createLocalDirectory(
        std::filesystem::path const& path,
        std::function<void(bool success, std::string const& info)> onComplete
    );

    /** @brief Shows a restore button in the queue header for a minimized SyncDialog.
     *         The button pulses once on call (and on subsequent calls) to catch the user's eye.
     *
     * @param onRestore Callback invoked when the user clicks the restore button.
     */
    void showMinimizedSync(std::function<void()> onRestore);

    /** @brief Hides the minimized-sync restore button (e.g. when the dialog is closed or reopened). */
    void hideMinimizedSync();

    void enqueueSyncScans(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        std::function<void(SharedData::ScanProgress const&)> onRemoteProgress,
        std::function<void(SharedData::ScanProgress const&)> onLocalProgress,
        std::function<void(SharedData::SyncScanResult)> onRemoteComplete,
        std::function<void(SharedData::SyncScanResult)> onLocalComplete
    );

    Nui::ElementRenderer operator()();

  private:
    template <typename OperationCard>
    void cancelOperation(OperationCard const& operation);
    void cancelAll();
    void togglePause();
    void askBackendToCancelAll();
    void changeAutoClean(bool doClean);

    void onOperationAdded(SharedData::OperationAdded const& added);
    void onDownloadProgress(SharedData::TransferProgress const& progress);
    void onUploadProgress(SharedData::TransferProgress const& progress);
    void onBulkDownloadProgress(SharedData::BulkProgress const& progress);
    void onBulkUploadProgress(SharedData::BulkProgress const& progress);
    void onDeleteProgress(SharedData::BulkDeleteProgress const& progress);
    void onScanProgress(SharedData::ScanProgress const& progress);
    void onOperationCompleted(Nui::val val);
    void onIsPaused(SharedData::ErrorOrSuccess<SharedData::IsPaused> const& result);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};