#pragma once

#include <nui-file-explorer/side.hpp>

#include <persistence/state_holder.hpp>
#include <persistence/state/session_options.hpp>
#include <persistence/state/termios.hpp>
#include <persistence/state/terminal_options.hpp>
#include <frontend/events/frontend_events.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <shared_data/directory_entry.hpp>
#include <ids/ids.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/utility/stabilize.hpp>
#include <nui/utility/move_detector.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class Session
{
  public:
    Session(
        Persistence::StateHolder* stateHolder,
        FrontendEvents* events,
        Persistence::SessionOptions sessionOptions,
        Persistence::UiOptions uiOptions,
        std::string initialName,
        std::optional<std::string> layoutName,
        InputDialog* newItemAskDialog,
        ConfirmDialog* confirmDialog,
        FilePropertyDialog* filePropertyDialog,
        std::function<void(Session const*)> closeSelf,
        std::function<std::string(std::string const&)> disambiguateTitle,
        bool visible
    );
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Session);

    Nui::ElementRenderer operator()();

    /**
     * @brief initiates shutdown of the session manager and calls onShutdown when done.
     * Do not use "from the inside", is only used by session manager to close a session.
     *
     * @param onShutdown callback to call when shutdown is complete
     */
    void shutdown(std::function<void()> onShutdown);

    std::string name() const;
    std::string layoutId() const;
    std::weak_ptr<Nui::Observed<std::string>> tabTitle() const;
    void visible(bool value);
    bool visible() const;

    std::optional<std::string> getProcessIdIfExecutingEngine() const;
    auto makeChannelElement() -> Nui::ElementRenderer;
    auto makeFileExplorerElement() -> Nui::ElementRenderer;
    auto makeOperationQueueElement() -> Nui::ElementRenderer;

    std::optional<nlohmann::json> getLayout() const;

    /**
     * @brief Used on windows to handle files dropped onto the session area.
     *
     * @param isLocalSide
     * @param entries
     * @param subdir
     */
    void
    onDrop(bool isLocalSide, std::vector<SharedData::DirectoryEntry> entries, std::optional<std::string> const& subdir);

  private:
    void onOpenSession(bool success, std::string const& info);
    void onOpenChannel(std::optional<Ids::ChannelId> channelId, std::string const& info);

    void onFileExplorerConnectionClose();
    void onTerminalConnectionLoss();
    void openSftp();
    void openLocalFilesystem();
    void closeSelf();
    void initializeLayout();

    void setupFileGrid();

    void onChannelClosedByUser(Ids::ChannelId const& channelId);

    void createExecutingEngine();
    void createSshEngine();

    NuiFileExplorer::Side& remoteFileGridSide();
    NuiFileExplorer::Side& localFileGridSide();

    RemoteSideModel& remoteSideModel();
    LocalSideModel& localSideModel();

    void loadLayoutExtras(nlohmann::json const& layoutExtra);

    Nui::ElementRenderer addTabMenu();
    void onConnectionLoss();
    void onLockedModeUserInput(Ids::ChannelId channelId, std::string const& input);
    void saveTerminalContents(std::filesystem::path const& file, std::vector<std::string> const& contents);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};