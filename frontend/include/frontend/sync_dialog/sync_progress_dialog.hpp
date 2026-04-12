#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <nui/utility/move_detector.hpp>

#include <filesystem>
#include <functional>
#include <memory>

class SyncProgressDialog
{
  public:
    SyncProgressDialog();
    ~SyncProgressDialog();
    SyncProgressDialog(SyncProgressDialog const&) = delete;
    SyncProgressDialog& operator=(SyncProgressDialog const&) = delete;
    SyncProgressDialog(SyncProgressDialog&&);
    SyncProgressDialog& operator=(SyncProgressDialog&&);

    /**
     * @brief Opens the progress dialog and runs a demo comparison sequence.
     *        Real comparison logic will replace the simulation later.
     *
     * @param localPath  Root of the local directory being compared.
     * @param remotePath Root of the remote directory being compared.
     * @param onDone     Called when comparison finishes (or is cancelled).
     *                   Pass nullptr to just close without a callback.
     */
    void open(
        std::filesystem::path localPath,
        std::filesystem::path remotePath,
        std::function<void()> onDone = {}
    );

    /** @brief Cancels an in-progress comparison and closes the dialog. */
    void cancel();

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};
