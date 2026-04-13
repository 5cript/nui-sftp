#pragma once

#include <shared_data/directory_entry.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

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

    /** @brief Opens the progress dialog and begins scanning both sides.
     *
     * @param localPath  Root of the local directory being compared.
     * @param remotePath Root of the remote directory being compared.
     * @param onDone     Called with (localEntries, remoteEntries) when both scans finish.
     *                   Not called if the dialog is cancelled.
     */
    void open(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        bool respectIgnoreFiles,
        std::function<void(
            std::vector<SharedData::DirectoryEntry> localEntries,
            std::vector<SharedData::DirectoryEntry> remoteEntries
        )> onDone
    );

    /** @brief Cancels an in-progress scan and closes the dialog. */
    void cancel();

    Nui::ElementRenderer operator()();

  private:
    OperationQueue* operationQueue_;
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
