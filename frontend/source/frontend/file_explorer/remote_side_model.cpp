#include <frontend/file_explorer/remote_side_model.hpp>

#include <utility/language.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <log/log.hpp>

#include <algorithm>
#include <iterator>

using namespace std::string_literals;

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
            .whatFor = language->get("remoteSideModel", "newDirectory"),
            .prompt = language->get("remoteSideModel", "enterNewDirectoryName"),
            .headerText = language->get("remoteSideModel", "createNewDirectory"),
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
            .whatFor = language->get("remoteSideModel", "newFile"),
            .prompt = language->get("remoteSideModel", "enterNewFileName"),
            .headerText = language->get("remoteSideModel", "createNewFile"),
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
        .headerText = language->get("remoteSideModel", "fileGridError"),
        .text = error,
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void RemoteSideModel::askNonEmptyDirectoryDeletions(
    std::vector<std::filesystem::path> confirmedToDelete,
    std::vector<std::filesystem::path> nonEmpties,
    std::vector<std::filesystem::path> filesAndEmptyDirs,
    std::size_t currentNonEmpty
)
{
    if (currentNonEmpty >= nonEmpties.size())
    {
        enqueueDeletes(std::move(confirmedToDelete), std::move(filesAndEmptyDirs));
        return;
    }

    const auto dir = nonEmpties[currentNonEmpty];

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Critical,
            .headerText = language->get("remoteSideModel", "nonEmptyDirectoriesFound"),
            .text = fmt::format(fmt::runtime(language->get("remoteSideModel", "nonEmptyDeleteAsk")), dir.string()),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::None |
                ConfirmDialog::Button::All | ConfirmDialog::Button::Cancel,
            .focusButton = ConfirmDialog::Button::No,
            .onClose = [this,
                           confirmedToDelete = std::move(confirmedToDelete),
                           nonEmpties = std::move(nonEmpties),
                           filesAndEmptyDirs = std::move(filesAndEmptyDirs),
                           currentNonEmpty,
                           dir](ConfirmDialog::Button button) mutable
            {
                if (button == ConfirmDialog::Button::Yes)
                {
                    confirmedToDelete.push_back(dir);
                    askNonEmptyDirectoryDeletions(
                        std::move(confirmedToDelete),
                        std::move(nonEmpties),
                        std::move(filesAndEmptyDirs),
                        currentNonEmpty + 1
                    );
                    return;
                }

                if (button == ConfirmDialog::Button::All)
                {
                    confirmedToDelete.push_back(dir);
                    for (; currentNonEmpty + 1 < nonEmpties.size(); ++currentNonEmpty)
                    {
                        confirmedToDelete.push_back(nonEmpties[currentNonEmpty + 1]);
                    }
                    enqueueDeletes(std::move(confirmedToDelete), std::move(filesAndEmptyDirs));
                    return;
                }

                if (button == ConfirmDialog::Button::No)
                {
                    askNonEmptyDirectoryDeletions(
                        std::move(confirmedToDelete),
                        std::move(nonEmpties),
                        std::move(filesAndEmptyDirs),
                        currentNonEmpty + 1
                    );
                    return;
                }

                if (button == ConfirmDialog::Button::None)
                {
                    enqueueDeletes(std::move(confirmedToDelete), std::move(filesAndEmptyDirs));
                    return;
                }

                Log::info("User cancelled deletion of non-empty directories.");
            }}
    );
}

void RemoteSideModel::enqueueDeletes(
    std::vector<std::filesystem::path> nonEmpties,
    std::vector<std::filesystem::path> filesAndEmptyDirs
)
{
    if (!filesAndEmptyDirs.empty())
    {
        operationQueue_->enqueueDelete(
            filesAndEmptyDirs,
            false,
            [this,
                nonEmpties = std::move(nonEmpties)](std::optional<std::vector<Ids::OperationId>> const& opIds) mutable
            {
                if (!opIds)
                {
                    Log::error("Failed to create delete operations");
                    confirmDialog_->open({
                        .state = ConfirmDialog::State::Negative,
                        .headerText = language->get("remoteSideModel", "deleteFailed"),
                        .text = language->get("remoteSideModel", "failedToCreateDeleteOperation"),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                    return;
                }

                if (!nonEmpties.empty())
                {
                    operationQueue_->enqueueDelete(
                        nonEmpties,
                        true,
                        [this](std::optional<std::vector<Ids::OperationId>> const& opIds) mutable
                        {
                            if (!opIds)
                            {
                                Log::error("Failed to create delete operations");
                                confirmDialog_->open({
                                    .state = ConfirmDialog::State::Negative,
                                    .headerText = language->get("remoteSideModel", "deleteFailed"),
                                    .text = language->get("remoteSideModel", "failedToCreateDeleteOperation"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }
                        }
                    );
                    return;
                }
            }
        );
        return;
    }
    else if (!nonEmpties.empty())
    {
        operationQueue_->enqueueDelete(
            nonEmpties,
            true,
            [this](std::optional<std::vector<Ids::OperationId>> const& opIds) mutable
            {
                if (!opIds)
                {
                    Log::error("Failed to create delete operations");
                    confirmDialog_->open({
                        .state = ConfirmDialog::State::Negative,
                        .headerText = language->get("remoteSideModel", "deleteFailed"),
                        .text = language->get("remoteSideModel", "failedToCreateDeleteOperation"),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                    return;
                }
            }
        );
        return;
    }
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
    std::string confirmText;
    if (itemSize > 1)
        confirmText = fmt::format(fmt::runtime(language->get("remoteSideModel", "deleteMultipleConfirm")), itemSize);
    else
        confirmText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "deleteSingleConfirm")), items.front().path.generic_string()
        );

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.state = ConfirmDialog::State::Information,
            .headerText = language->get("remoteSideModel", "deleteItemsQuestion"),
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

                std::vector<NuiFileExplorer::Item> directories;
                std::vector<NuiFileExplorer::Item> files;
                for (auto item : items)
                {
                    item.path = currentPath_.value() / item.path;
                    if (item.type == NuiFileExplorer::Item::Type::Directory)
                        directories.push_back(item);
                    else
                        files.push_back(item);
                }

                Log::info("Deleting items");
                fileEngine_->remove(
                    files,
                    directories,
                    [this](bool success)
                    {
                        if (!success)
                        {
                            Log::error("Failed to delete files");
                            confirmDialog_->open({
                                .state = ConfirmDialog::State::Negative,
                                .headerText = language->get("remoteSideModel", "deleteFailed"),
                                .text = language->get("remoteSideModel", "failedToDeleteItems"),
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                            return;
                        }
                        // Refresh list from server:
                        onRefresh();
                    },
                    [this](
                        std::vector<std::filesystem::path> filesAndEmptyDirs,
                        std::vector<std::filesystem::path> nonEmpties
                    )
                    {
                        askNonEmptyDirectoryDeletions({}, std::move(nonEmpties), std::move(filesAndEmptyDirs), 0);
                    }
                );
            }}
    );
}

void RemoteSideModel::enqueueSingleDownload(
    NuiFileExplorer::Item const& remoteItem,
    NuiFileExplorer::Item const& localItem,
    bool allowOverwrite,
    bool insertRefresh
)
{
    operationQueue_->enqueueDownload(
        remoteItem,
        localItem,
        [this](std::optional<Ids::OperationId> const& opId)
        {
            if (!opId)
            {
                Log::error("Failed to create download operation");
                confirmDialog_->open({
                    .state = ConfirmDialog::State::Negative,
                    .headerText = language->get("remoteSideModel", "downloadFailed"),
                    .text = language->get("remoteSideModel", "failedToCreateDownloadOperation"),
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
    std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> downloadItems,
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

    const auto fileToCheckFor = downloadItems[index].second.path.generic_string();
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
                .headerText = language->get("remoteSideModel", "checkFileExistenceFailed"),
                .text = language->get("remoteSideModel", "invalidResponseFromBackend"),
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

        Log::info("Downloading '{}' to '{}'.", item.first.path.generic_string(), item.second.path.generic_string());
        if (exists && !overwriteNever)
        {
            Log::info("File already exists: {}", item.second.path.generic_string());
            confirmDialog_->open(
                {.state = ConfirmDialog::State::Information,
                    .headerText = language->get("remoteSideModel", "fileAlreadyExistsOverwrite"),
                    .text = language->get("remoteSideModel", "allowOverwritingFile"),
                    .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                        ConfirmDialog::Button::None,
                    .focusButton = ConfirmDialog::Button::No,
                    .listItems = {{.text = item.second.path.generic_string(), .description = "File already exists"}},
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
                                "Skipping download of existing file: {}",
                                downloadItems[index].second.path.generic_string()
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

void RemoteSideModel::onDropExternal(
    std::vector<NuiFileExplorer::Item> const& items,
    std::optional<std::string> const& subDir,
    bool issueWebkitWarning
)
{
    // Not implemented yet.
    (void)items;
    (void)subDir;
    (void)issueWebkitWarning;
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

    std::string confirmText;
    if (itemsSize > 1)
    {
        confirmText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "downloadConfirmIfMultiple")),
            itemsSize,
            destinationDir.generic_string()
        );
    }
    else
    {
        confirmText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "downloadConfirmIfSingle")),
            items.front().path.generic_string(),
            destinationDir.generic_string()
        );
    }

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

                std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> downloadItems;
                std::transform(
                    items.begin(),
                    items.end(),
                    std::back_inserter(downloadItems),
                    [this, destinationDir](auto const& item)
                    {
                        NuiFileExplorer::Item localItem = item;
                        NuiFileExplorer::Item remoteItem = item;
                        localItem.path = destinationDir / item.path.filename();
                        remoteItem.path = currentPath_.value() / item.path;
                        return std::make_pair(remoteItem, localItem);
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
        .whatFor = language->get("remoteSideModel", "rename"),
        .prompt =
            fmt::format(fmt::runtime(language->get("remoteSideModel", "renamePrompt")), item.path.filename().string()),
        .headerText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "renameWithItem")), item.path.filename().string()
        ),
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