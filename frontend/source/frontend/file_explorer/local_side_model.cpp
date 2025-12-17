#include <frontend/file_explorer/local_side_model.hpp>

#include <log/log.hpp>

LocalSideModel::LocalSideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog)
    : SideModel{std::move(uiOptions), confirmDialog, inputDialog}
{}

void LocalSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{}
void LocalSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{}
const std::vector<NuiFileExplorer::Item>& LocalSideModel::items() const
{
    return items_;
}
void LocalSideModel::onPathChange(std::filesystem::path const& path)
{}
void LocalSideModel::onRefresh()
{}
void LocalSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{}
void LocalSideModel::onTransfer(std::vector<NuiFileExplorer::Item> const& items)
{
    // TODO:

    // Log::info("Upload items requested: {}", items.size());
    // for (const auto& item : items)
    // {
    //     Log::debug("Item: {}", item.path.generic_string());
    // }

    // const auto destinationDir = remoteFileGridSide().path();

    // const auto itemsSize = items.size();
    // std::string confirmText = fmt::format(
    //     "Are you sure you want to upload {} {} {}?:",
    //     itemsSize > 1 ? "the following" : items.front().path.generic_string(),
    //     itemsSize == 1 ? "" : std::to_string(itemsSize),
    //     itemsSize == 1 ? "" : "items");

    // std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    // for (const auto& item : items)
    // {
    //     listItems.push_back({item.path.generic_string(), ""});
    // }

    // impl_->confirmDialog->open(
    //     {.state = ConfirmDialog::State::Information,
    //      .headerText = "Upload Items?",
    //      .text = confirmText,
    //      .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
    //      .listItems = listItems,
    //      .onClose = [this, items, destinationDir](ConfirmDialog::Button button) {
    //          if (button != ConfirmDialog::Button::Yes)
    //          {
    //              Log::info("Upload items cancelled");
    //              return;
    //          }

    //          // pair <remote, local>
    //          std::vector<std::pair<std::filesystem::path, std::filesystem::path>> uploadItems;
    //          std::transform(
    //              items.begin(), items.end(), std::back_inserter(uploadItems), [destinationDir](auto const& item) {
    //                  return std::make_pair(destinationDir / item.path.filename(), item.path);
    //              });

    //          Log::info("Uploading items");
    //          for (const auto& item : uploadItems)
    //          {
    //              Log::info("Uploading '{}' to '{}'", item.second.generic_string(), item.first.generic_string());
    //              impl_->operationQueue.enqueueUpload(
    //                  item.first /* remote */,
    //                  item.second /* local */,
    //                  [this](std::optional<Ids::OperationId> const& opId) {
    //                      if (!opId)
    //                      {
    //                          Log::error("Failed to create upload operation");
    //                          impl_->confirmDialog->open({
    //                              .state = ConfirmDialog::State::Negative,
    //                              .headerText = "Upload Failed",
    //                              .text = "Failed to create upload operation",
    //                              .buttons = ConfirmDialog::Button::Ok,
    //                          });
    //                          return;
    //                      }
    //                      Log::info("Upload operation created with id: {}", opId->value());
    //                  });
    //          }
    //      }});
}
void LocalSideModel::onRename(NuiFileExplorer::Item const& item)
{}
void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{}
void LocalSideModel::onError(std::string const& error)
{
    Log::error("File grid error (local side): {}", error);
    confirmDialog_->open({
        .state = ConfirmDialog::State::Negative,
        .headerText = "File Grid Error (Local Side)",
        .text = error,
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void LocalSideModel::navigateTo(std::filesystem::path const& path)
{}