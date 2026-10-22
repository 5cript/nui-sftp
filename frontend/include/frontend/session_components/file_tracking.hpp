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
 * @brief Shows the file tracker of a session, which lives in the backend as "FileTracker::<sessionId>": its watches,
 *        its questions and the orphaned download directories. Forwards what the user asks for and decides nothing.
 */
class FileTrackingPanel
{
  public:
    explicit FileTrackingPanel(ConfirmDialog* confirmDialog);
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
     * @brief Asks the tracker to remove @p localPath, a download of the instance @p instanceId the user decided not to
     *        open, with its watch.
     */
    void discard(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath);

    Nui::ElementRenderer operator()();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
