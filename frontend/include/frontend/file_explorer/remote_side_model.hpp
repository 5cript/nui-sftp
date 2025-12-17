#pragma once

#include <frontend/file_explorer/side_model.hpp>
#include <frontend/dialog/confirm_dialog.hpp>
#include <frontend/dialog/input_dialog.hpp>
#include <frontend/terminal/file_engine.hpp>
#include <persistence/state_holder.hpp>

#include <nui-file-explorer/side_model_interface.hpp>

class RemoteSideModel : public SideModel
{
  public:
    RemoteSideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog);

    void onActivateItem(NuiFileExplorer::Item const& item) override;
    void onNewItem(NuiFileExplorer::Item::Type type) override;
    const std::vector<NuiFileExplorer::Item>& items() const override;
    void onPathChange(std::filesystem::path const& path) override;
    void onRefresh() override;
    void onDelete(std::vector<NuiFileExplorer::Item> const& items) override;
    void onTransfer(std::vector<NuiFileExplorer::Item> const& items) override;
    void onRename(NuiFileExplorer::Item const& item) override;
    void onProperties(NuiFileExplorer::Item const& item) override;
    void onError(std::string const& error) override;

    void navigateTo(std::filesystem::path const& path) override;
    void onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries);
};