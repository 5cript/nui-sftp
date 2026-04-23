#pragma once

#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_summary.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <functional>
#include <memory>

class ConfirmDialog;
class OperationQueue;

class SyncDialog
{
  public:
    SyncDialog(ConfirmDialog* confirmDialog, OperationQueue* operationQueue);
    ~SyncDialog();
    SyncDialog(SyncDialog const&) = delete;
    SyncDialog& operator=(SyncDialog const&) = delete;
    SyncDialog(SyncDialog&&);
    SyncDialog& operator=(SyncDialog&&);

    /**
     * @brief Opens the dialog against a backend-resident sync session whose first
     *        diff has already completed.
     *
     * @param provider Non-owning handle to the session; must outlive the dialog.
     * @param summary  The @ref DiffSummary returned from the initial recompute.
     *                 Used to seed footer totals and drive root-tree layout.
     * @param localPath Local scan root (for path column formatting only — data is
     *                  served lazily from @p provider).
     * @param remotePath Remote scan root.
     */
    void open(
        BackendSyncProvider* provider,
        SharedData::Sync::DiffSummary summary,
        std::filesystem::path localPath,
        std::filesystem::path remotePath
    );

    /**
     * @brief Scan + diff settings snapshot handed to the Recompare callback.
     *        The scan flags (respectIgnoreFiles/recursive/ignoreHidden) govern
     *        the backend listing; the diff options are the first recompute's
     *        inputs.
     */
    struct RecompareRequest
    {
        bool respectIgnoreFiles{true};
        bool recursive{true};
        bool ignoreHidden{false};
        SharedData::Sync::DiffOptions diffOptions{};
    };

    /**
     * @brief Called when the user clicks the Recompare button.  Session wires
     *        this to close the existing provider, re-run the scan-and-diff flow
     *        with the given settings, then @ref open the dialog again against a
     *        fresh session.
     */
    void setOnRecompareRequested(std::function<void(RecompareRequest)> callback);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
