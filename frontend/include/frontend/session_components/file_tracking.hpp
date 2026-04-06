#pragma once

#include <persistence/state_holder.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <filesystem>

class OperationQueue;

class FileTrackingPanel
{
  public:
    FileTrackingPanel(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FileTrackingPanel);

    /** @brief Connect to a live session's operation queue. */
    void activate(OperationQueue* operationQueue, Ids::SessionId sessionId);

    /** @brief Disconnect from the session; clears all watches and entries. */
    void deactivate();

    /**
     * @brief Register a new watched entry after a priority download has been initiated.
     *
     * @param instanceId  The FileTracking instance ID returned by createInstance.
     * @param instanceDir The local temporary directory for this instance.
     * @param remotePath  Absolute remote path that was downloaded.
     * @param localPath   Local path inside instanceDir where the file/dir lives.
     */
    void startWatching(
        Ids::InstanceId const& instanceId,
        std::filesystem::path const& instanceDir,
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath
    );

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
