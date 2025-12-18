#include <frontend/file_explorer/local_side_model.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>

LocalSideModel::LocalSideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog)
    : SideModel{std::move(uiOptions), confirmDialog, inputDialog}
{}

void LocalSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    // TODO:
}
void LocalSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{
    inputDialog_->open(
        {.whatFor = "New item",
         .prompt = "Enter the name of the new item",
         .headerText = "Create a new item",
         .isPassword = false,
         .onConfirm = [this, type](std::optional<std::string> const& name) {
             if (!name)
                 return;

             if (name->find('/') != std::string::npos)
             {
                 Log::error("Invalid item name (cannot contain slashes): {}", *name);
                 return;
             }
             Log::info("Creating new item: {}", *name);

             if (type == NuiFileExplorer::Item::Type::Directory)
             {
                 Nui::val args = Nui::val::object();
                 args.set("path", (currentPath_ / *name).generic_string());

                 Nui::RpcClient::callWithBackChannel(
                     "RpcFilesystem::createDirectory",
                     [this](Nui::val val) {
                         if (!val.hasOwnProperty("success"))
                         {
                             Log::error("Invalid response from RpcFilesystem::createDirectory");
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Create Directory Failed",
                                 .text = "Invalid response from backend",
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }

                         const auto success = val["success"].as<bool>();
                         if (!success)
                         {
                             const auto error = val["error"].as<std::string>();
                             Log::error("Failed to create directory: {}", error);
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Create Directory Failed",
                                 .text = error,
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }

                         Log::info("Directory created successfully");
                         // Refresh list:
                         navigateTo(currentPath_);
                     },
                     args);
             }
             else if (type == NuiFileExplorer::Item::Type::Regular)
             {
                 Nui::val args = Nui::val::object();
                 args.set("path", (currentPath_ / *name).generic_string());

                 Nui::RpcClient::callWithBackChannel(
                     "RpcFilesystem::createFile",
                     [this](Nui::val val) {
                         if (!val.hasOwnProperty("success"))
                         {
                             Log::error("Invalid response from RpcFilesystem::createFile");
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Create File Failed",
                                 .text = "Invalid response from backend",
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }

                         const auto success = val["success"].as<bool>();
                         if (!success)
                         {
                             const auto error = val["error"].as<std::string>();
                             Log::error("Failed to create file: {}", error);
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Create File Failed",
                                 .text = error,
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }

                         Log::info("File created successfully");
                         // Refresh list:
                         navigateTo(currentPath_);
                     },
                     args);
             }
             else
             {
                 confirmDialog_->open({
                     .state = ConfirmDialog::State::Negative,
                     .headerText = "Create Item Failed",
                     .text = "Unsupported item type",
                     .buttons = ConfirmDialog::Button::Ok,
                 });
             }
         }});
}
void LocalSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    Nui::val args = Nui::val::object();
    args.set("paths", Nui::val::array());

    for (const auto& item : items)
    {
        args["paths"].call<void>("push", item.path.generic_string());
    }

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::removeSome",
        [this](Nui::val val) {
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::remove");
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "Delete Files Failed",
                    .text = "Invalid response from backend",
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = val["error"].as<std::string>();
                Log::error("Failed to delete files: {}", error);
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "Delete Files Failed",
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            Log::info("Files deleted successfully");
            // Refresh list:
            navigateTo(currentPath_);
        },
        args);
}
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
{
    // TODO: ? huh what to what?
}
void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    Nui::val args = Nui::val::object();
    args.set("path", item.path.generic_string());

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::properties",
        [this](Nui::val val) {
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::properties");
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "Create Directory Failed",
                    .text = "Invalid response from backend",
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = val["error"].as<std::string>();
                Log::error("Failed to get file properties: {}", error);
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "Get File Properties Failed",
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto type = static_cast<std::filesystem::file_type>(val["type"].as<int>());
            const auto size = val["size"].as<std::uint64_t>();

            std::string propertiesText = fmt::format(
                "Type: {}\nSize: {} bytes",
                SharedData::fileTypeToString(SharedData::fileTypeFromStdFilesystemType(type)),
                size);

            confirmDialog_->open({
                .state = ConfirmDialog::State::Information,
                .headerText = "File Properties",
                .text = propertiesText,
                .buttons = ConfirmDialog::Button::Ok,
            });
        },
        args);
}
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
{
    Nui::val args = Nui::val::object();
    args.set("path", path.generic_string());

    Log::info("Local navigating to: {}", path.generic_string());
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::listFiles",
        [this](Nui::val val) {
            Log::debug("Received directory listing response");
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::properties");
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "List Files Failed",
                    .text = "Invalid response from backend",
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = val["error"].as<std::string>();
                Log::error("Failed to list files: {}", error);
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = "List Files Failed",
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            std::vector<SharedData::DirectoryEntry> directoryEntries;
            const auto files = val["files"];
            for (const auto& file : files)
            {
                directoryEntries.push_back(
                    SharedData::DirectoryEntry{
                        .path = file["path"].as<std::string>(),
                        .type = SharedData::fileTypeFromStdFilesystemType(
                            static_cast<std::filesystem::file_type>(file["type"].as<int>())),
                        .size = file["size"].as<std::uint64_t>(),
                    });
            }

            onDirectoryListing(directoryEntries);
        },
        args);
}