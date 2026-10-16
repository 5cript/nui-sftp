#pragma once

#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <functional>
#include <memory>

class OperationQueue;

class SyncProgressDialog
{
  public:
    explicit SyncProgressDialog(OperationQueue* operationQueue);
    ~SyncProgressDialog();
    SyncProgressDialog(SyncProgressDialog const&) = delete;
    SyncProgressDialog& operator=(SyncProgressDialog const&) = delete;
    SyncProgressDialog(SyncProgressDialog&&);
    SyncProgressDialog& operator=(SyncProgressDialog&&);

    /**
     * @brief Opens the progress dialog, runs both scans via @p provider, then
     *        immediately triggers a backend diff.  @p onDone fires after both the
     *        scans and the first diff have landed, passing the @ref DiffSnapshot
     *        the frontend should seed its trees from.
     *
     *        The dialog keeps showing the Comparing spinner during the diff walk;
     *        light diffs finish within a frame so the phase effectively flashes.
     *
     * @param provider         The sync session, held until the first diff landed, a scan failed or the
     *                         dialog was cancelled.
     * @param initialOptions   The diff options to apply for the first recompute.
     * @param onDone           Called exactly once on success with the provider, which this dialog lets go
     *                         of then.  Not invoked on cancel or failure.
     */
    void open(
        std::shared_ptr<BackendSyncProvider> provider,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        bool respectIgnoreFiles,
        bool recursive,
        bool ignoreHidden,
        SharedData::Sync::DiffOptions initialOptions,
        std::function<void(std::shared_ptr<BackendSyncProvider>, SharedData::Sync::DiffSnapshot)> onDone
    );

    /**
     * @brief Cancels the in-progress scan or diff and closes the dialog.  Safe to
     *        call before @ref open or after @ref open completes (no-op in those
     *        cases).
     */
    void cancel();

    Nui::ElementRenderer operator()();

  private:
    OperationQueue* operationQueue_;
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
