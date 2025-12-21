#include <frontend/file_explorer/local_side_model.hpp>
#include <log/log.hpp>

#include <nui/rpc.hpp>

LocalSideModel::LocalSideModel(Persistence::UiOptions uiOptions, ConfirmDialog* confirmDialog, InputDialog* inputDialog)
    : SideModel{std::move(uiOptions), confirmDialog, inputDialog}
{}

void LocalSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    // TODO: what about files?:
    if (item.type != NuiFileExplorer::Item::Type::Directory)
        return;

    if (item.path == ".")
        return;
    if (item.path == "..")
        return navigateTo(currentPath_.value().parent_path());

    Log::debug("Local onActivateItem: {}.", item.path.generic_string());
    navigateTo(currentPath_.value() / item.path);
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
                 args.set("path", (currentPath_.value() / *name).generic_string());

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
                         navigateTo(currentPath_.value());
                     },
                     args);
             }
             else if (type == NuiFileExplorer::Item::Type::Regular)
             {
                 Nui::val args = Nui::val::object();
                 args.set("path", (currentPath_.value() / *name).generic_string());

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
                         navigateTo(currentPath_.value());
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
    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Information,
         .headerText = "Delete Items?",
         .text = "Are you sure you want to delete the selected items?",
         .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
         .listItems = listItems,
         .onClose = [this, items](ConfirmDialog::Button button) {
             if (button != ConfirmDialog::Button::Yes)
             {
                 Log::info("Delete items cancelled");
                 return;
             }

             Nui::val args = Nui::val::object();
             args.set("paths", Nui::val::array());

             for (const auto& item : items)
             {
                 args["paths"].call<void>("push", (*currentPath_ / item.path).generic_string());
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
                     navigateTo(currentPath_.value());
                 },
                 args);
         }});
}
void LocalSideModel::onTransfer(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    Log::info("Upload items requested: {}", items.size());
    for (const auto& item : items)
    {
        Log::debug("Item: {}", item.path.generic_string());
    }

    const auto destinationDir = remoteModel_->currentPath().value();
    const auto sourceDir = currentPath_.value();

    const auto itemsSize = items.size();
    std::string confirmText = fmt::format(
        "Are you sure you want to upload {} {} {} to {}?:",
        itemsSize > 1 ? "the following" : items.front().path.generic_string(),
        itemsSize == 1 ? "" : std::to_string(itemsSize),
        itemsSize == 1 ? "" : "items",
        destinationDir.generic_string());

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Information,
         .headerText = "Upload Items?",
         .text = confirmText,
         .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
         .listItems = listItems,
         .onClose = [this, items, destinationDir, sourceDir](ConfirmDialog::Button button) {
             if (button != ConfirmDialog::Button::Yes)
             {
                 Log::info("Upload items cancelled");
                 return;
             }

             // pair <remote, local>
             std::vector<std::pair<std::filesystem::path, std::filesystem::path>> uploadItems;
             std::transform(
                 items.begin(),
                 items.end(),
                 std::back_inserter(uploadItems),
                 [destinationDir, sourceDir](auto const& item) {
                     return std::make_pair(destinationDir / item.path.filename(), sourceDir / item.path.filename());
                 });

             Log::info("Uploading items");
             for (const auto& item : uploadItems)
             {
                 Log::info("Uploading '{}' to '{}'", item.second.generic_string(), item.first.generic_string());
                 operationQueue_->enqueueUpload(
                     item.first /* remote */,
                     item.second /* local */,
                     [this](std::optional<Ids::OperationId> const& opId) {
                         if (!opId)
                         {
                             Log::error("Failed to create upload operation");
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Upload Failed",
                                 .text = "Failed to create upload operation",
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }
                         Log::info("Upload operation created with id: {}", opId->value());
                     });
             }
         }});
}
void LocalSideModel::onRename(NuiFileExplorer::Item const& item)
{
    Log::info("Rename item requested: {}", item.path.generic_string());

    auto doRename = [item, this](std::string const& newName) {
        Nui::val args = Nui::val::object();
        args.set("oldPath", (*currentPath_ / item.path).generic_string());
        args.set("newPath", (*currentPath_ / newName).generic_string());

        Nui::RpcClient::callWithBackChannel(
            "RpcFilesystem::rename",
            [this](Nui::val val) {
                if (!val.hasOwnProperty("success"))
                {
                    Log::error("Invalid response from RpcFilesystem::rename");
                    confirmDialog_->open({
                        .state = ConfirmDialog::State::Negative,
                        .headerText = "Rename Failed",
                        .text = "Invalid response from backend",
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                    return;
                }

                const auto success = val["success"].as<bool>();
                if (!success)
                {
                    const auto error = val["error"].as<std::string>();
                    Log::error("Failed to rename item: {}", error);
                    confirmDialog_->open({
                        .state = ConfirmDialog::State::Negative,
                        .headerText = "Rename Failed",
                        .text = error,
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                    return;
                }

                Log::info("Item renamed successfully");
                // Refresh list:
                navigateTo(currentPath_.value());
            },
            args);
    };

    inputDialog_->open({
        .whatFor = "Rename",
        .prompt = "Enter the new name for " + item.path.filename().string(),
        .headerText = "Rename " + item.path.filename().string(),
        .isPassword = false,
        .onConfirm =
            [doRename](std::optional<std::string> const& name) {
                if (!name)
                    return;

                Log::info("Renaming item to: {}", *name);
                doRename(*name);
            },
    });
}
void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    Nui::val args = Nui::val::object();
    args.set("path", *currentPath_ / item.path.generic_string());

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

void LocalSideModel::onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries)
{
    if (currentPath_.value().has_parent_path())
    {
        directoryEntries->insert(
            directoryEntries->begin(),
            SharedData::DirectoryEntry{
                .path = "..",
                .type = SharedData::FileType::Directory,
                .size = 0,
            });
    }
    SideModel::onDirectoryListing(directoryEntries);
}

void LocalSideModel::navigateTo(std::filesystem::path const& path)
{
    auto lexicallyNormal = path.lexically_normal();
    Log::info("Local navigating to: {}", lexicallyNormal.generic_string());
    preNavigatePath_ = currentPath_.value();
    currentPath_ = lexicallyNormal;

    Nui::val args = Nui::val::object();
    args.set("path", currentPath_.value().generic_string());
    args.set("fileNameOnly", true);

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

void LocalSideModel::setRemoteModel(SideModel* model)
{
    remoteModel_ = model;
}

bool LocalSideModel::isComplete() const
{
    return remoteModel_ != nullptr && SideModel::isComplete();
}