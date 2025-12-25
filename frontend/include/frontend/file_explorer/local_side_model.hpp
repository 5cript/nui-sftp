#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>

class LocalSideModel : public SideModel
{
  public:
    LocalSideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog);

    void onActivateItem(NuiFileExplorer::Item const& item) override;
    void onNewItem(NuiFileExplorer::Item::Type type) override;
    void onDelete(std::vector<NuiFileExplorer::Item> const& items) override;
    void onTransfer(std::vector<NuiFileExplorer::Item> const& items) override;
    void onRename(NuiFileExplorer::Item const& item) override;
    void onProperties(NuiFileExplorer::Item const& item) override;
    void onError(std::string const& error) override;

    bool isLeft() const override
    {
        return true;
    }
    void setRemoteModel(SideModel* model);
    bool isComplete() const override;
    void navigateTo(std::filesystem::path const& path) override;
    void onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries) override;

  private:
    SideModel* remoteModel_{nullptr};
};