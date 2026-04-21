#pragma once

#include <persistence/state/session_options.hpp>
#include <persistence/state/ui_options.hpp>
#include <shared_data/directory_entry.hpp>

#include <nui-file-explorer/side.hpp>

#include <nui/event_system/observed_value.hpp>
#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/dom/element.hpp>

#include <roar/detail/pimpl_special_functions.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Persistence
{
    class StateHolder;
}
namespace NuiFileExplorer
{
    class FileGrid;
}
struct FrontendEvents;
class ConfirmDialog;
class InputDialog;
class FilePropertyDialog;
class ArchiveTransferDialog;
class LocalSideModel;
class RemoteSideModel;

/**
 * @brief Wraps `NuiFileExplorer::FileGrid` into a session_components-style
 *        widget with its own Observed DOM element and SFTP-bound view blocker.
 *        Extracted from Session as part of the session.cpp refactor (Step 4).
 */
class FileExplorerPanel
{
  public:
    /**
     * @brief Wiring for FileExplorerPanel.  Bundled so the call site does not
     *        degrade into a positional argument marathon as the surface grows.
     */
    struct Params
    {
        Persistence::StateHolder* stateHolder = nullptr;
        FrontendEvents* events = nullptr;
        ConfirmDialog* confirmDialog = nullptr;
        InputDialog* inputDialog = nullptr;
        FilePropertyDialog* filePropertyDialog = nullptr;
        ArchiveTransferDialog* archiveTransferDialog = nullptr;

        /** @brief Session name used to scope favorites persistence on the remote side. */
        std::string sessionName;

        /** @brief UI options captured at construction (favorites, hidden-files, etc.). */
        Persistence::UiOptions uiOptions{};

        /**
         * @brief Engine configuration.  The engine variant decides whether a
         *        remote side is built (SSH = both sides, local-shell/executing
         *        = local only).  Remote favorites also live inside this.
         */
        Persistence::SessionOptions engineOptions{};

        /**
         * @brief Non-owning pointer to the Session's reactive lost-connection
         *        flag.  Drives the SFTP-bound view blocker's visibility the
         *        grid dims whenever the owning Session is reconnecting.
         */
        Nui::Observed<bool>* isInLostConnectionState = nullptr;
    };

    explicit FileExplorerPanel(Params params);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(FileExplorerPanel);

    /**
     * @brief Late-phase wiring sets up the grid error handler, item-update
     *        functions, local/remote cross-linking, and the favorites
     *        persistence callbacks.  Called once, from Session's constructor
     *        body, after the FileExplorerPanel exists.
     */
    void setup();

    /**
     * @brief Attaches the caller-provided synchronize callback to both sides
     *        of the grid.  Used by Session::setupFileGrid to wire in the
     *        sync-dialog + progress-dialog open flow, which themselves stay
     *        on Session.
     */
    void setOnSynchronize(std::function<void(std::filesystem::path, std::filesystem::path)> onSync);

    /**
     * @brief Factory for the file-explorer DOM subtree: the grid plus its
     *        per-panel view blocker.
     */
    Nui::ElementRenderer makeFileExplorerElement();

    /**
     * @brief Handles drag-and-drop of external files onto the session area
     *        (Windows).  Drops onto the local side show a "not implemented"
     *        dialog; drops onto the remote side hand the entries to the
     *        LocalSideModel::onTransfer path as an upload.
     */
    void onDrop(
        bool isLocalSide,
        std::vector<SharedData::DirectoryEntry> entries,
        std::optional<std::string> const& subdir
    );

    /** @brief Drops layout metadata for the given session layout id. */
    void dropLayoutMetadata(std::string const& sessionLayoutId);

    NuiFileExplorer::FileGrid& fileGrid();
    NuiFileExplorer::Side& localFileGridSide();
    NuiFileExplorer::Side* remoteFileGridSide();
    LocalSideModel& localSideModel();
    RemoteSideModel* remoteSideModel();

    /**
     * @brief Raw access to the Observed element handle used by the Lumino
     *        file-explorer factory / deleter and by the Session's
     *        tab-add-menu listener.
     */
    Nui::Observed<std::shared_ptr<Nui::Dom::Element>>& elementObservable();

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
