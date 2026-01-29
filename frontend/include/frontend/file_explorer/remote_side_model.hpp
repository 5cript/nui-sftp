#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/file_property_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>
#include <nui-file-explorer/path_suggestion_cache.hpp>

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
    void onRename(NuiFileExplorer::Item const& item) override;
    void onProperties(NuiFileExplorer::Item const& item) override;
    void onError(std::string const& error) override;

    bool isLeft() const override
    {
        return false;
    }
    void navigateTo(std::filesystem::path const& path) override;
    void setLocalModel(SideModel* model);
    bool isComplete() const override;

    void generatePathBoxSuggestions(
        std::filesystem::path const& path,
        int maxSuggestions,
        std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
    ) override;

  private:
    void enqueueRefresh(bool otherModel);

    void downloadItemsConfirmed(
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> downloadItems,
        std::size_t index = 0,
        bool overwriteNever = false,
        bool overwriteAlways = false
    );

    void enqueueSingleDownload(
        std::filesystem::path const& remotePath,
        std::filesystem::path const& localPath,
        bool allowOverwrite,
        bool insertRefresh
    );

  private:
    SideModel* localModel_{nullptr};
    NuiFileExplorer::PathSuggestionCache pathSuggestionCache_;
};