#pragma once

#include <frontend/sync_dialog/backend_sync_provider.hpp>
#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_snapshot.hpp>
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
     * @brief Settings the scans of a sync session ran with. Changing them needs a recompare.
     */
    struct ScanSettings
    {
        bool respectIgnoreFiles{true};
        bool recursive{true};
        bool ignoreHidden{false};
    };

    /**
     * @brief Opens the dialog against a backend-resident sync session whose first
     *        diff has already completed.
     *
     * @param provider The sync session, kept until the dialog closes or opens with another one. Callbacks of
     *                 executions under a previous provider are dropped then.
     * @param snapshot The result of the initial recompute: summary and section roots.
     * @param localPath Local scan root (for path column formatting only, data is
     *                  served lazily from @p provider).
     * @param remotePath Remote scan root.
     * @param scanned Settings the scans of @p provider ran with.
     */
    void open(
        std::shared_ptr<BackendSyncProvider> provider,
        SharedData::Sync::DiffSnapshot snapshot,
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        ScanSettings scanned
    );

    /**
     * @brief Scan + diff settings snapshot handed to the Recompare callback.
     */
    struct RecompareRequest
    {
        ScanSettings scan{};
        SharedData::Sync::DiffOptions diffOptions{};
    };

    /**
     * @brief Called when the user clicks the Recompare button.  Session wires
     *        this to close the existing provider, re-run the scan-and-diff flow
     *        with the given settings, then @ref open the dialog again against a
     *        fresh session.
     */
    void setOnRecompareRequested(std::function<void(RecompareRequest)> callback);

    /** @brief The scan and diff settings the dialog's switches currently show. */
    RecompareRequest currentSettings() const;

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
