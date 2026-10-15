#pragma once

#include <persistence/state_holder.hpp>
#include <persistence/state/sftp_options.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/events/frontend_events.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <filesystem>

class OperationQueue;

class FileTrackingPanel
{
  public:
    FileTrackingPanel(Persistence::StateHolder* stateHolder, FrontendEvents* events, ConfirmDialog* confirmDialog);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FileTrackingPanel);

    /**
     * @brief Connect to a live session's operation queue.
     * @param downloadTempSuffix The suffix the session's downloads write under until complete, see
     *        Persistence::effectiveTempFileSuffix. The watches ignore those files.
     */
    void activate(OperationQueue* operationQueue, Ids::SessionId sessionId, std::string downloadTempSuffix);

    /** @brief The suffix of the session's downloads in progress, given to activate(). */
    std::string const& downloadTempSuffix() const;

    /** @brief Disconnect from the session; clears all watches and entries. */
    void deactivate();

    /**
     * @brief Register a new watched entry after a priority download has been initiated.
     *
     * @param instanceId  The FileTracking instance ID returned by createInstance.
     * @param instanceDirectory The local temporary directory for this instance.
     * @param remotePath  Absolute remote path that was downloaded.
     * @param localPath   Local path inside instanceDirectory where the file/dir lives.
     */
    void startWatching(
        Ids::InstanceId const& instanceId,
        Paths::LocalPath const& instanceDirectory,
        Paths::RemotePath const& remotePath,
        Paths::LocalPath const& localPath
    );

    /**
     * @brief Removes @p localPath, a download of the instance @p instanceId the user decided not to open, and ends
     *        its watch like Stop does when it has one. Once nothing else is left in the instance, it ends and its
     *        directory is removed, so it leaves no orphan.
     */
    void discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
