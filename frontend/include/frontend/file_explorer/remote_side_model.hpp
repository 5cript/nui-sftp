#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/path_suggestion_cache.hpp>

class FileTrackingPanel;

class RemoteSideModel : public SideModel
{
  public:
    RemoteSideModel(
        Persistence::UiOptions uiOptions,
        ConfirmDialog* confirmDialog,
        InputDialog* inputDialog,
        FilePropertyDialog* filePropertyDialog
    );

    void onActivateItem(NuiFileExplorer::Item const& item) override;
    void onNewItem(NuiFileExplorer::Item::Type type) override;
    void onDelete(std::vector<NuiFileExplorer::Item> const& items) override;
    void onTransfer(std::vector<NuiFileExplorer::Item> const& items, std::optional<std::string> const& subDir) override;
    std::vector<NuiFileExplorer::ContextMenuItem> contextMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems) override;
    void onDropExternal(
        std::vector<NuiFileExplorer::Item> const& items,
        std::optional<std::string> const& subDir,
        bool issueWebkitWarning
    ) override;
    void onRename(NuiFileExplorer::Item const& item) override;
    void onProperties(NuiFileExplorer::Item const& item) override;
    void onError(std::string const& error) override;

    bool isLeft() const override
    {
        return false;
    }
    void navigateTo(std::filesystem::path const& path) override;
    void setLocalModel(SideModel* model);
    void setFileTracking(FileTrackingPanel* fileTracking);
    bool isComplete() const override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    void enqueueRefresh(bool otherModel);

    void downloadItemsConfirmed(
        std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> downloadItems,
        std::size_t index = 0,
        bool overwriteNever = false,
        bool overwriteAlways = false
    );

    void enqueueSingleDownload(
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem,
        bool allowOverwrite,
        bool insertRefresh
    );

    void askNonEmptyDirectoryDeletions(
        std::vector<std::filesystem::path> confirmedToDelete,
        std::vector<std::filesystem::path> nonEmpties,
        std::vector<std::filesystem::path> filesAndEmptyDirs,
        std::size_t currentNonEmpty
    );

    void
    enqueueDeletes(std::vector<std::filesystem::path> nonEmpties, std::vector<std::filesystem::path> filesAndEmptyDirs);

    void continueDeletion();
    void onWatchDownloadAndOpen(std::vector<NuiFileExplorer::Item> const& items, bool openWith);

  private:
    SideModel* localModel_{nullptr};
    FileTrackingPanel* fileTracking_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
};