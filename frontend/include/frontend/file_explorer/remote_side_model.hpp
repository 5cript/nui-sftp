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

    void engine(std::unique_ptr<FileEngine> fileEngine);
    FileEngine* engine();

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

  private:
    void enqueueRefresh(bool otherModel);

  private:
    std::unique_ptr<FileEngine> fileEngine_{nullptr};
    SideModel* localModel_{nullptr};
};