#pragma once

#include <shared_data/directory_entry.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

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

    /** @brief Opens the dialog with pre-scanned directory listings and computes the diff.
     *
     * @param localPath    Absolute path of the local directory root.
     * @param remotePath   Absolute path of the remote directory root.
     * @param localEntries Flat entry list from a LocalScanOperation (fullPaths pre-computed).
     * @param remoteEntries Flat entry list from a ScanOperation (fullPaths pre-computed).
     */
    void open(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::vector<SharedData::DirectoryEntry> localEntries,
        std::vector<SharedData::DirectoryEntry> remoteEntries
    );

    /** @brief Sets the callback invoked when the user clicks Recompare.
     *         The callback receives local/remote paths and an onResult function that should be
     *         called once the new comparison is complete, passing the resulting entry lists.
     *
     * @param callback Callable with signature
     *        (std::filesystem::path local, std::filesystem::path remote,
     *         std::function<void(localEntries, remoteEntries)> onResult).
     */
    void setOnRecompare(
        std::function<void(
            std::filesystem::path,
            std::filesystem::path,
            bool respectIgnoreFiles,
            std::function<void(
                std::vector<SharedData::DirectoryEntry>,
                std::vector<SharedData::DirectoryEntry>
            )>
        )> callback
    );

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
