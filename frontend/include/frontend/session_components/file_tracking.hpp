#pragma once

#include <frontend/dialog/confirm_dialog.hpp>
#include <ids/ids.hpp>
#include <paths/paths.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * @brief The File Tracking panel of a session: the outliner of its watches and orphaned copies beside the detail of
 *        the selected one. Shows the file tracker that lives in the backend as "FileTracker::<sessionId>", forwards
 *        what the user asks for and decides nothing. Also drives the attention of its dock tab.
 */
class FileTrackingPanel
{
  public:
    struct Params
    {
        /** @brief Asks before deleting orphaned copies and shows errors of opening local items. */
        ConfirmDialog* confirmDialog = nullptr;
        /** @brief The session's layout id, naming its dock panel for the tab attention. */
        std::string sessionLayoutId = {};
    };

    explicit FileTrackingPanel(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FileTrackingPanel);

    /**
     * @brief Shows the tracker of the session @p sessionId from now on.
     */
    void activate(Ids::SessionId sessionId);

    /**
     * @brief Shows nothing any more, the session has ended.
     */
    void deactivate();

    /**
     * @brief Asks the tracker to download @p remotePath into a directory of its own, and to watch it there when
     *        @p watch is set. @p onOpened gets the copy, @p onRefused the translated reason when there is none.
     *        Neither is called when the download did not end well, its card in the queue tells why.
     */
    void open(
        Paths::RemotePath const& remotePath,
        std::uint64_t sizeBytes,
        bool watch,
        std::function<void(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)> onOpened,
        std::function<void(std::string const& reason)> onRefused
    );

    /**
     * @brief Asks the tracker whether @p item could be uploaded into @p directory and watched now, before the user is
     *        asked anything about the upload. @p onWatchable gets the item canonical, as the watch will report it;
     *        @p onRefused gets the translated reason.
     */
    void checkUploadAndWatch(
        Paths::LocalPath const& item,
        Paths::RemotePath const& directory,
        std::function<void(Paths::LocalPath const& canonicalItem)> onWatchable,
        std::function<void(std::string const& reason)> onRefused
    );

    /**
     * @brief Asks the tracker to watch @p item, upload it to @p remotePath and mirror its changes from then on.
     *        @p onRefused gets the translated reason when it does not.
     */
    void uploadAndWatch(
        Paths::LocalPath const& item,
        Paths::RemotePath const& remotePath,
        std::function<void(std::string const& reason)> onRefused
    );

    /**
     * @brief Asks the tracker to remove @p localPath, a download of the instance @p instanceId the user decided not to
     *        open, with its watch.
     */
    void discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath);

    /**
     * @brief What Reveal on Server does: shows @p path in the session's remote file explorer side.
     */
    void setRevealOnServer(std::function<void(Paths::RemotePath const& path)> revealOnServer);

    /**
     * @brief The user activated the dock tab: jumps to the newest conflict when conflicts arrived since the last
     *        jump. Does not sync.
     */
    void onTabActivated();

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
