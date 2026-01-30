#include <frontend/file_explorer/remote_side_model.hpp>

#include <log/log.hpp>

#include <algorithm>
#include <iterator>

RemoteSideModel::RemoteSideModel(
    Persistence::UiOptions uiOptions,
    ConfirmDialog* confirmDialog,
    InputDialog* inputDialog,
    FilePropertyDialog* filePropertyDialog
)
    : SideModel{std::move(uiOptions), confirmDialog, inputDialog, filePropertyDialog}
    , pathSuggestionCache_{
          [this](
              std::filesystem::path const& dirPath,
              std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
          )
          {
              CHECK_COMPLETE();

              fileEngine_->listDirectory(
                  dirPath,
                  [onResultsAvailable =
                          std::move(onResultsAvailable)](std::optional<std::vector<SharedData::DirectoryEntry>> entries)
                  {
                      if (!entries)
                      {
                          Log::error("Failed to list files from remote side.");
                          onResultsAvailable({});
                          return;
                      }

                      std::vector<std::filesystem::path> listing{};
                      listing.reserve(entries->size());
                      for (auto&& entry : std::move(*entries))
                      {
                          if (entry.isDirectory())
                              listing.emplace_back(std::move(entry).path);
                      }

                      onResultsAvailable(listing);
                  }
              );
          },
          0.,
      }
{}

void RemoteSideModel::setLocalModel(SideModel* model)
{
    localModel_ = model;
}

bool RemoteSideModel::isComplete() const
{
    return localModel_ != nullptr && SideModel::isComplete();
}

void RemoteSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    // TODO: what about files?:
    if (item.type != NuiFileExplorer::Item::Type::Directory)
        return;

    if (item.path == ".")
        return;
    if (item.path == "..")
        return navigateTo(currentPath_.value().parent_path());

    Log::debug("Remote onActivateItem: {}.", item.path.generic_string());
    navigateTo(currentPath_.value() / item.path);
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
            .onConfirm = [this](std::optional<std::string> const& name)
            {
                if (!name)
                    return;

                Log::info("Creating new directory: {}", *name);
                if (name->find('/') != std::string::npos)
                {
                    Log::error("Invalid directory name (cannot contain slashes): {}", *name);
                    return;
                }
                fileEngine_->createDirectory(
                    currentPath_.value() / *name,
                    [this](bool success)
                    {
                        if (!success)
                        {
                            Log::error("Failed to create directory");
                            return;
                        }
                        // Refresh list from server:
                        navigateTo(currentPath_.value());
                    }
                );
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
            .onConfirm = [this](std::optional<std::string> const& name)
            {
                if (!name)
                    return;

                Log::info("Creating new file: {}", *name);
                if (name->find('/') != std::string::npos)
                {
                    Log::error("Invalid file name (cannot contain slashes): {}", *name);
                    return;
                }
                fileEngine_->createFile(
                    currentPath_.value() / *name,
                    [this](bool success)
                    {
                        if (!success)
                        {
                            Log::error("Failed to create file");
                            return;
                        }
                        // Refresh list from server:
                        navigateTo(currentPath_.value());
                    }
                );
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
        itemSize == 1 ? "" : "items"
    );

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
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items](ConfirmDialog::Button button)
            {
                if (button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Delete items cancelled");
                    return;
                }

                std::vector<std::filesystem::path> fullPaths;
                std::transform(
                    items.begin(),
                    items.end(),
                    std::back_inserter(fullPaths),
                    [this](auto const& item)
                    {
                        return (currentPath_.value() / item.path).generic_string();
                    }
                );

                Log::info("Deleting items");
                fileEngine_->remove(
                    fullPaths,
                    [this](bool success)
                    {
                        if (!success)
                        {
                            Log::error("Failed to delete files");
                            confirmDialog_->open({
                                .state = ConfirmDialog::State::Negative,
                                .headerText = "Delete Failed",
                                .text = "Failed to delete one or more files / directories.",
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                            return;
                        }
                        // Refresh list from server:
                        onRefresh();
                    }
                );
            }}
    );
}

void RemoteSideModel::enqueueSingleDownload(
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    bool allowOverwrite,
    bool insertRefresh
)
{
    operationQueue_->enqueueDownload(
        remotePath,
        localPath,
        [this](std::optional<Ids::OperationId> const& opId)
        {
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
        },
        allowOverwrite,
        insertRefresh
    );
}

void RemoteSideModel::downloadItemsConfirmed(
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> downloadItems,
    std::size_t index,
    bool overwriteNever,
    bool overwriteAlways
)
{
    if (index == downloadItems.size())
        return;

    if (overwriteAlways)
    {
        for (; index < downloadItems.size(); ++index)
            enqueueSingleDownload(
                downloadItems[index].first,
                downloadItems[index].second,
                overwriteAlways,
                index + 1 == downloadItems.size()
            );
        return;
    }

    const auto fileToCheckFor = downloadItems[index].second.generic_string();
    auto onExistsResponse = [this, downloadItems = std::move(downloadItems), index, overwriteNever, overwriteAlways](
                                Nui::val response
                            ) mutable
    {
        Nui::WebApi::Console::log("RpcFilesystem::exists val: ", response);

        auto const& item = downloadItems[index];

        if (!response.hasOwnProperty("success"))
        {
            Log::error("Invalid response from RpcFilesystem::exists");
            confirmDialog_->open({
                .state = ConfirmDialog::State::Negative,
                .headerText = "Check File Existence Failed",
                .text = "Invalid response from backend",
                .buttons = ConfirmDialog::Button::Ok,
            });
            return;
        }

        const auto success = response["success"].as<bool>();
        if (!success || !response.hasOwnProperty("exists") || response["exists"].isNull() ||
            response["exists"].isUndefined())
        {
            const auto error = response["error"].as<std::string>();
            Log::error("Failed to check file existence: {}", error);
            confirmDialog_->open({
                .state = ConfirmDialog::State::Negative,
                .headerText = "Check File Existence Failed",
                .text = error,
                .buttons = ConfirmDialog::Button::Ok,
            });
            return;
        }

        const auto exists = response["exists"].as<bool>();

        Log::info("Downloading '{}' to '{}'.", item.first.generic_string(), item.second.generic_string());
        if (exists && !overwriteNever)
        {
            Log::info("File already exists: {}", item.second.generic_string());
            confirmDialog_->open(
                {.state = ConfirmDialog::State::Information,
                    .headerText = "File already exists, overwrite?",
                    .text = "Allow overwriting this file?  Do note that bulk downloads in subdirectories might cause "
                            "multiple files to be overwritten.",
                    .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                        ConfirmDialog::Button::None,
                    .focusButton = ConfirmDialog::Button::No,
                    .listItems = {{.text = item.second.generic_string(), .description = "File already exists"}},
                    .onClose = [this, downloadItems = std::move(downloadItems), index, overwriteNever, overwriteAlways](
                                   ConfirmDialog::Button button
                               ) mutable
                    {
                        if (button == ConfirmDialog::Button::Yes)
                        {
                            enqueueSingleDownload(
                                downloadItems[index].first,
                                downloadItems[index].second,
                                true,
                                index + 1 == downloadItems.size()
                            );
                            downloadItemsConfirmed(
                                std::move(downloadItems), index + 1, overwriteNever, overwriteAlways
                            );
                        }
                        else if (button == ConfirmDialog::Button::No)
                        {
                            Log::info(
                                "Skipping download of existing file: {}", downloadItems[index].second.generic_string()
                            );
                            downloadItemsConfirmed(
                                std::move(downloadItems), index + 1, overwriteNever, overwriteAlways
                            );
                        }
                        else if (button == ConfirmDialog::Button::All)
                        {
                            Log::info("Overwriting all existing files from now on.");
                            enqueueSingleDownload(
                                downloadItems[index].first,
                                downloadItems[index].second,
                                true,
                                index + 1 == downloadItems.size()
                            );
                            downloadItemsConfirmed(std::move(downloadItems), index + 1, overwriteNever, true);
                        }
                        else if (button == ConfirmDialog::Button::None)
                        {
                            Log::info("Skipping all existing files from now on.");
                            downloadItemsConfirmed(std::move(downloadItems), index + 1, true, overwriteAlways);
                        }
                    }}
            );
            return;
        }

        if (!exists)
            enqueueSingleDownload(item.first, item.second, false, index + 1 == downloadItems.size());
        downloadItemsConfirmed(std::move(downloadItems), index + 1, overwriteNever, overwriteAlways);
    };

    Nui::val args = Nui::val::object();
    args.set("path", fileToCheckFor);

    Nui::RpcClient::callWithBackChannel("RpcFilesystem::exists", onExistsResponse, args);
}

void RemoteSideModel::onTransfer(
    std::vector<NuiFileExplorer::Item> const& items,
    std::optional<std::string> const& subDir
)
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

    auto destinationDir = localModel_->currentPath().value();
    if (subDir)
        destinationDir /= *subDir;

    std::string confirmText = fmt::format(
        "Are you sure you want to download {} {} {} to {}?:",
        itemsSize > 1 ? "the following" : items.front().path.generic_string(),
        itemsSize == 1 ? "" : std::to_string(itemsSize),
        itemsSize == 1 ? "" : "items",
        destinationDir.generic_string()
    );

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
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items, destinationDir](ConfirmDialog::Button button)
            {
                if (button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Download items cancelled");
                    return;
                }

                std::vector<std::pair<std::filesystem::path, std::filesystem::path>> downloadItems;
                std::transform(
                    items.begin(),
                    items.end(),
                    std::back_inserter(downloadItems),
                    [this, destinationDir](auto const& item)
                    {
                        return std::make_pair(currentPath_.value() / item.path, destinationDir / item.path.filename());
                    }
                );

                Log::info("Downloading items.");
                downloadItemsConfirmed(downloadItems);
            }}
    );
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
        .onConfirm = [this, item](std::optional<std::string> const& name)
        {
            if (!name)
                return;

            Log::info("Renaming item to: {}", *name);

            std::filesystem::path fullFrom = currentPath_.value() / item.path;
            std::filesystem::path fullTo = currentPath_.value() / *name;

            fileEngine_->rename(
                fullFrom,
                fullTo,
                [this, fullFrom, fullTo](bool success)
                {
                    if (!success)
                    {
                        Log::error(
                            "Failed to rename item '{}' to '{}'", fullFrom.generic_string(), fullTo.generic_string()
                        );
                        return;
                    }
                    Log::info("Renamed item '{}' to '{}'", fullFrom.generic_string(), fullTo.generic_string());
                    onRefresh();
                }
            );
        },
    });
}

void RemoteSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Properties requested: {}", item.path.generic_string());

    filePropertyDialog_->open(static_cast<SharedData::DirectoryEntry const&>(item));
}

void RemoteSideModel::navigateTo(std::filesystem::path const& path)
{
    CHECK_COMPLETE();

    auto lexicallyNormal = path.lexically_normal();
    Log::info("Navigating to: {}", lexicallyNormal.generic_string());
    preNavigatePath_ = currentPath_.value();
    currentPath_ = lexicallyNormal;
    fileEngine_->listDirectory(
        currentPath_.value(), std::bind(&RemoteSideModel::onDirectoryListing, this, std::placeholders::_1)
    );
}

void RemoteSideModel::generatePathBoxSuggestions(
    std::filesystem::path const& path,
    int maxSuggestions,
    std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
)
{
    CHECK_COMPLETE();
    pathSuggestionCache_.generateSuggestions(
        path,
        maxSuggestions,
        [onResultsAvailable, path](std::vector<std::filesystem::path> suggestions)
        {
            for (auto& suggestion : suggestions)
                suggestion = path.parent_path() / suggestion;
            onResultsAvailable(suggestions);
        }
    );
}