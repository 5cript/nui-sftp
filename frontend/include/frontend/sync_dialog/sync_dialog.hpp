#pragma once

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
     * @brief Opens the dialog for the given local / remote directory pair.
     *        Populates dummy items until comparison logic is wired up.
     *
     * @param localPath  Absolute path of the local directory root.
     * @param remotePath Absolute path of the remote directory root.
     */
    void open(std::filesystem::path localPath, std::filesystem::path remotePath);

    /**
     * @brief Sets a callback invoked when the user clicks Recompare.
     *        The callback receives the local path, remote path, and an onDone function
     *        that should be called once the comparison is complete to repopulate items.
     *
     * @param callback Callable with signature
     *        (std::filesystem::path local, std::filesystem::path remote, std::function<void()> onDone).
     */
    void setOnRecompare(
        std::function<void(std::filesystem::path, std::filesystem::path, std::function<void()>)> callback
    );

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
