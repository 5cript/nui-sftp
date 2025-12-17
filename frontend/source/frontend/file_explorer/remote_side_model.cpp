#include <frontend/file_explorer/remote_side_model.hpp>

#include <log/log.hpp>

RemoteSideModel::RemoteSideModel(
    Persistence::UiOptions uiOptions,
    ConfirmDialog* confirmDialog,
    InputDialog* inputDialog)
    : SideModel{std::move(uiOptions), confirmDialog, inputDialog}
{}

void RemoteSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    // TODO: what about files?:
    if (item.type != NuiFileExplorer::Item::Type::Directory)
        return;

    if (item.path == ".")
        return;
    if (item.path == "..")
        return navigateTo(currentPath_.parent_path());

    navigateTo(currentPath_ / item.path);
}
void RemoteSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{
    CHECK_COMPLETE();

    Log::info("New item requested: {}", static_cast<int>(type));
    if (type == NuiFileExplorer::Item::Type::Directory)
    {
        inputDialog_->open({
            .whatFor = "New directory",
            .prompt = "Enter the name of the new directory",
            .headerText = "Create a new directory",
            .isPassword = false,
            .onConfirm =
                [this](std::optional<std::string> const& name) {
                    if (!name)
                        return;

                    Log::info("Creating new directory: {}", *name);
                    if (name->find('/') != std::string::npos)
                    {
                        Log::error("Invalid directory name (cannot contain slashes): {}", *name);
                        return;
                    }
                    fileEngine_->createDirectory(currentPath_ / *name, [this](bool success) {
                        if (!success)
                        {
                            Log::error("Failed to create directory");
                            return;
                        }
                        // Refresh list from server:
                        navigateTo(currentPath_);
                    });
                },
        });
    }
    else if (type == NuiFileExplorer::Item::Type::Regular)
    {
        inputDialog_->open({
            .whatFor = "New file",
            .prompt = "Enter the name of the new file",
            .headerText = "Create a new file",
            .isPassword = false,
            .onConfirm =
                [this](std::optional<std::string> const& name) {
                    if (!name)
                        return;

                    Log::info("Creating new file: {}", *name);
                    if (name->find('/') != std::string::npos)
                    {
                        Log::error("Invalid file name (cannot contain slashes): {}", *name);
                        return;
                    }
                    fileEngine_->createFile(currentPath_ / *name, [this](bool success) {
                        if (!success)
                        {
                            Log::error("Failed to create file");
                            return;
                        }
                        // Refresh list from server:
                        navigateTo(currentPath_);
                    });
                },
        });
    }
    else
    {
        // TODO: create file
    }
}
void RemoteSideModel::onError(std::string const& error)
{
    Log::error("File grid error (remote side): {}", error);
    confirmDialog_->open({
        .state = ConfirmDialog::State::Negative,
        .headerText = "File Grid Error (Remote Side)",
        .text = error,
        .buttons = ConfirmDialog::Button::Ok,
    });
}
const std::vector<NuiFileExplorer::Item>& RemoteSideModel::items() const
{
    return items_;
}
void RemoteSideModel::onPathChange(std::filesystem::path const& path)
{
    navigateTo(path);
}
void RemoteSideModel::onRefresh()
{
    navigateTo(currentPath_);
}

void RemoteSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    using namespace std::string_literals;

    Log::info("Delete items requested: {}", items.size());
    for (const auto& item : items)
    {
        Log::info("Item: {}", item.path.generic_string());
    }

    if (items.empty())
    {
        Log::error("No items selected for deletion");
        return;
    }

    const auto itemSize = items.size();
    std::string confirmText = fmt::format(
        "Are you sure you want to delete {} {} {}?:",
        itemSize > 1 ? "the following" : items.front().path.generic_string(),
        itemSize == 1 ? ""s : std::to_string(itemSize),
        itemSize == 1 ? "" : "items");

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Information,
         .headerText = "Delete Items?",
         .text = confirmText,
         .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
         .listItems = listItems,
         .onClose = [items](ConfirmDialog::Button button) {
             if (button != ConfirmDialog::Button::Yes)
             {
                 Log::info("Delete items cancelled");
                 return;
             }

             Log::info("Deleting items");
             // TODO: ...
         }});
}
void RemoteSideModel::onTransfer(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    Log::info("Download items requested: {}", items.size());
    for (const auto& item : items)
    {
        Log::debug("Item: {}", item.path.generic_string());
    }

    if (items.empty())
    {
        Log::error("No items selected for download");
        return;
    }

    const auto itemsSize = items.size();
    std::string confirmText = fmt::format(
        "Are you sure you want to download {} {} {}?:",
        itemsSize > 1 ? "the following" : items.front().path.generic_string(),
        itemsSize == 1 ? "" : std::to_string(itemsSize),
        itemsSize == 1 ? "" : "items");

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Information,
         .headerText = "Download Items?",
         .text = confirmText,
         .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
         .listItems = listItems,
         .onClose = [this, items](ConfirmDialog::Button button) {
             if (button != ConfirmDialog::Button::Yes)
             {
                 Log::info("Download items cancelled");
                 return;
             }

             std::vector<std::pair<std::filesystem::path, std::filesystem::path>> downloadItems;
             std::transform(items.begin(), items.end(), std::back_inserter(downloadItems), [this](auto const& item) {
                 // TODO: Proper target path handling:
                 return std::make_pair(currentPath_ / item.path, "D:/DownloadTemp" / item.path.filename());
             });

             Log::info("Downloading items");
             for (const auto& item : downloadItems)
             {
                 Log::info("Downloading '{}' to '{}'", item.first.generic_string(), item.second.generic_string());
                 operationQueue_->enqueueDownload(
                     item.first, item.second, [this](std::optional<Ids::OperationId> const& opId) {
                         if (!opId)
                         {
                             Log::error("Failed to create download operation");
                             confirmDialog_->open({
                                 .state = ConfirmDialog::State::Negative,
                                 .headerText = "Download Failed",
                                 .text = "Failed to create download operation",
                                 .buttons = ConfirmDialog::Button::Ok,
                             });
                             return;
                         }
                         Log::info("Download operation created with id: {}", opId->value());
                     });
             }
         }});
}
void RemoteSideModel::onRename(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Rename item requested: {}", item.path.generic_string());

    inputDialog_->open({
        .whatFor = "Rename",
        .prompt = "Enter the new name for " + item.path.filename().string(),
        .headerText = "Rename " + item.path.filename().string(),
        .isPassword = false,
        .onConfirm =
            [item](std::optional<std::string> const& name) {
                if (!name)
                    return;

                Log::info("Renaming item to: {}", *name);
                // TODO: ...
            },
    });
}
void RemoteSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Properties requested: {}", item.path.generic_string());

    // TODO: ...
}
void RemoteSideModel::onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries)
{
    CHECK_COMPLETE();

    if (!directoryEntries)
    {
        Log::error("Failed to list directory");
        // undo the navigation:
        currentPath_ = preNavigatePath_;
        navigateTo(currentPath_);
        return;
    }

    std::erase_if(*directoryEntries, [](auto const& entry) {
        return entry.path.filename() == ".";
    });

    std::vector<NuiFileExplorer::Item> items{};
    std::transform(
        begin(*directoryEntries), end(*directoryEntries), std::back_inserter(items), [this](auto const& entry) {
            return NuiFileExplorer::Item{
                .path = entry.path,
                .icon = [&entry, this]() -> std::string {
                    const auto type = static_cast<NuiFileExplorer::Item::Type>(entry.type);
                    if (type == NuiFileExplorer::Item::Type::Directory)
                        return "nui://app.example/icons/folder_main.png";
                    if (type == NuiFileExplorer::Item::Type::BlockDevice)
                        return "nui://app.example/icons/hard_drive.png";

                    if (uiOptions_.fileGridExtensionIcons.contains(entry.path.extension().string()))
                    {
                        return "nui://app.example/" +
                            uiOptions_.fileGridExtensionIcons.at(entry.path.extension().string());
                    }

                    return "nui://app.example/icons/file.png";
                }(),
                .type = static_cast<NuiFileExplorer::Item::Type>(entry.type),
                .permissions = entry.permissions,
                .ownerId = entry.uid,
                .groupId = entry.gid,
                .atime = entry.atime,
                .size = entry.size,
            };
        });

    items_ = std::move(items);
    if (refreshCallback_)
        refreshCallback_(true);
}
void RemoteSideModel::navigateTo(std::filesystem::path const& path)
{
    CHECK_COMPLETE();

    auto lexicallyNormal = path.lexically_normal();
    Log::info("Navigating to: {}", lexicallyNormal.generic_string());
    preNavigatePath_ = currentPath_;
    currentPath_ = lexicallyNormal;
    fileEngine_->listDirectory(
        currentPath_, std::bind(&RemoteSideModel::onDirectoryListing, this, std::placeholders::_1));
}