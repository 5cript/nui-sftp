#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/file_explorer_texts.hpp>
#include <frontend/file_explorer/place_names.hpp>
#include <frontend/session_components/file_tracking.hpp>

#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/open-folder.hpp>
#include <ui5-sap-icons/icons/open-command-field.hpp>
#include <ui5-sap-icons/icons/synchronize.hpp>
#include <ui5-sap-icons/icons/show.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/detail-view.hpp>
#include <ui5-sap-icons/icons/add-favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>

#include <utility/language.hpp>
#include <utility/user_directories.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <script-nui-components/popup_menu.hpp>
#include <log/log.hpp>
#include <ids/ids.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <nui/rpc.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

using namespace std::string_literals;

RemoteSideModel::RemoteSideModel(
    Persistence::UiOptions uiOptions,
    std::vector<std::string> initialFavorites,
    ConfirmDialog* confirmDialog,
    InputDialog* inputDialog,
    FilePropertyDialog* filePropertyDialog,
    ArchiveTransferDialog* archiveTransferDialog
)
    : SideModel{
          std::move(uiOptions), confirmDialog, inputDialog, filePropertyDialog, archiveTransferDialog
      }
    , pathSuggestionCache_{
          [this](
              std::filesystem::path const& dirPath,
              std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
          )
          {
              CHECK_COMPLETE();

              fileEngine_->listDirectory(
                  dirPath,
                  [this, onResultsAvailable = std::move(onResultsAvailable)](
                      std::optional<std::vector<SharedData::DirectoryEntry>> entries, std::string const& info
                  )
                  {
                      if (!entries)
                      {
                          Log::error("Failed to list files from remote side: {}", info);
                          confirmDialog_->open({
                              .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                              .headerText = "",
                              .text = info,
                              .buttons = ConfirmDialog::Button::Ok,
                          });
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
    , favorites_{std::make_shared<Nui::Observed<std::vector<std::filesystem::path>>>(
          [&initialFavorites]()
          {
              std::vector<std::filesystem::path> paths;
              paths.reserve(initialFavorites.size());
              for (auto const& str : initialFavorites)
                  paths.emplace_back(str);
              return paths;
          }()
      )}
{}

void RemoteSideModel::setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback)
{
    onFavoritesChanged_ = std::move(callback);
}

std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> RemoteSideModel::favorites() const
{
    return favorites_;
}

void RemoteSideModel::addFavorite(std::filesystem::path const& path)
{
    auto& favs = favorites_->value();
    if (std::find(favs.begin(), favs.end(), path) != favs.end())
        return;
    favorites_->push_back(path);

    if (onFavoritesChanged_)
    {
        std::vector<std::string> strs;
        strs.reserve(favorites_->value().size());
        for (auto const& fav : favorites_->value())
            strs.push_back(fav.generic_string());
        onFavoritesChanged_(std::move(strs));
    }
}

void RemoteSideModel::removeFavorite(std::filesystem::path const& path)
{
    auto& favs = favorites_->value();
    const auto it = std::find(favs.begin(), favs.end(), path);
    if (it == favs.end())
        return;
    favs.erase(it);
    favorites_->modifyNow();

    if (onFavoritesChanged_)
    {
        std::vector<std::string> strs;
        strs.reserve(favorites_->value().size());
        for (auto const& fav : favorites_->value())
            strs.push_back(fav.generic_string());
        onFavoritesChanged_(std::move(strs));
    }
}

std::vector<NuiFileExplorer::ContextMenuItem>
RemoteSideModel::contextMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems)
{
    namespace Snc = ScriptNuiComponents;
    const bool hasItems = !selectedItems.empty();
    const bool singleItem = selectedItems.size() == 1;

    std::vector<NuiFileExplorer::ContextMenuItem> items;

    if (singleItem && selectedItems.front().isDirectoryLike() && !selectedItems.front().isParentLink)
    {
        const auto& selPath = selectedItems.front().fullPath;
        const auto& favs = favorites_->value();
        const bool isFav = std::find(favs.begin(), favs.end(), selPath) != favs.end();
        items.push_back(
            Snc::PopupMenu::item(
                language->get("fileExplorer", "contextMenu", isFav ? "removeFromFavorites" : "addToFavorites"),
                isFav ? Ui5Icons::unfavorite() : Ui5Icons::add_favorite(),
                [this, selPath, isFav]()
                {
                    if (isFav)
                        removeFavorite(selPath);
                    else
                        addFavorite(selPath);
                }
            )
        );
        items.push_back(Snc::PopupMenu::separator());
    }

    // "Open" variants on the remote side ultimately route the downloaded file through the
    // local Opener (xdg-desktop-portal / ShellExecute), so gate them on the same capability
    // the LocalSideModel probes at startup.
    const bool canOpen = localModel_ ? localModel_->openerCapabilities().canOpenFile : true;

    const std::vector<NuiFileExplorer::ContextMenuItem> baseItems = {
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "download"),
            Ui5Icons::download(),
            [this, selectedItems]()
            {
                onTransfer(selectedItems, std::nullopt);
            },
            !hasItems
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "downloadAsArchive"),
            Ui5Icons::download(),
            [this, selectedItems]()
            {
                onTransferAsArchive(selectedItems);
            },
            !hasItems || archiveTransferDialog_ == nullptr
        ),
        Snc::PopupMenu::separator(),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "open"),
            Ui5Icons::open_folder(),
            [this, selectedItems]()
            {
                onDownloadAndOpen(selectedItems.front(), false);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWith"),
            Ui5Icons::open_command_field(),
            [this, selectedItems]()
            {
                onDownloadAndOpen(selectedItems.front(), true);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openAndWatch"),
            Ui5Icons::show(),
            [this, selectedItems]()
            {
                onWatchDownloadAndOpen(selectedItems.front(), false);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWithAndWatch"),
            Ui5Icons::show(),
            [this, selectedItems]()
            {
                onWatchDownloadAndOpen(selectedItems.front(), true);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::separator(),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "delete"),
            Ui5Icons::delete_(),
            [this, selectedItems]()
            {
                onDelete(selectedItems);
            },
            !hasItems
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "rename"),
            Ui5Icons::edit(),
            [this, selectedItems]()
            {
                onRename(selectedItems.front());
            },
            !singleItem
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "properties"),
            Ui5Icons::detail_view(),
            [this, selectedItems]()
            {
                onProperties(selectedItems.front());
            },
            !singleItem
        ),
    };

    items.insert(items.end(), baseItems.begin(), baseItems.end());
    return items;
}

void RemoteSideModel::setLocalModel(LocalSideModel* model)
{
    localModel_ = model;
}

void RemoteSideModel::setFileTracking(FileTrackingPanel* fileTracking)
{
    fileTracking_ = fileTracking;
}

std::expected<Paths::RemotePath, Paths::PathError> RemoteSideModel::currentRemotePath() const
{
    return Paths::RemotePath::make(listedPath_.generic_string());
}

std::expected<Paths::RemotePath, Paths::PathError> RemoteSideModel::remotePathOf(NuiFileExplorer::Item const& item) const
{
    if (item.isParentLink)
        return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::DotOrDotDot, .input = ".."});
    return Paths::PathSegment::make(item.name)
        .and_then(
            [&item](Paths::PathSegment const&)
            {
                return Paths::RemotePath::make(item.fullPath.generic_string());
            }
        );
}

std::expected<Paths::RemotePath, Paths::PathError> RemoteSideModel::childOfCurrentPath(std::string_view name) const
{
    return currentRemotePath().and_then(
        [name](Paths::RemotePath const& directory)
        {
            return Paths::PathSegment::make(name).transform(
                [&directory](Paths::PathSegment const& segment)
                {
                    return directory / segment;
                }
            );
        }
    );
}

bool RemoteSideModel::isComplete() const
{
    return localModel_ != nullptr && SideModel::isComplete();
}

void RemoteSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    if (!item.isDirectoryLike())
    {
        const bool canOpen = localModel_ ? localModel_->openerCapabilities().canOpenFile : true;
        if (fileTracking_ != nullptr && canOpen)
            onDownloadAndOpen(item, false);
        return;
    }

    if (item.path == ".")
        return;

    // The item's own path, the side may already be loading another directory.
    Log::debug("Remote onActivateItem: {}.", item.displayName());
    navigateTo(item.fullPath.empty() ? currentPath_.value() / item.name : item.fullPath);
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

                if (!isValidItemName(*name))
                    return showInvalidItemName(*name);
                auto path = childOfCurrentPath(*name);
                if (!path)
                    return showPathRefused(language->get("remoteSideModel", "createNewDirectory"), path.error());
                Log::info("Creating new directory: {}", path->value());
                fileEngine_->createDirectory(
                    *path,
                    [this](bool success, std::string const& info)
                    {
                        if (!success)
                        {
                            Log::error("Failed to create directory: {}", info);
                            confirmDialog_->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = "",
                                .text = info,
                                .buttons = ConfirmDialog::Button::Ok,
                            });
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

                if (!isValidItemName(*name))
                    return showInvalidItemName(*name);
                auto path = childOfCurrentPath(*name);
                if (!path)
                    return showPathRefused(language->get("remoteSideModel", "createNewFile"), path.error());
                Log::info("Creating new file: {}", path->value());
                fileEngine_->createFile(
                    *path,
                    [this](bool success, std::string const& info)
                    {
                        if (!success)
                        {
                            Log::error("Failed to create file: {}", info);
                            confirmDialog_->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = "",
                                .text = info,
                                .buttons = ConfirmDialog::Button::Ok,
                            });
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

void RemoteSideModel::onError(NuiFileExplorer::Error const& fileExplorerError)
{
    const auto error = fileExplorerErrorText(fileExplorerError);
    Log::error("File grid error (remote side): {}", error);
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("remoteSideModel", "fileGridError"),
        .text = error,
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void RemoteSideModel::askNonEmptyDirectoryDeletions(
    std::vector<Paths::RemotePath> confirmedToDelete,
    std::vector<Paths::RemotePath> nonEmpties,
    std::vector<Paths::RemotePath> filesAndEmptyDirs,
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
        {.styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("remoteSideModel", "nonEmptyDirectoriesFound"),
            .text = fmt::format(fmt::runtime(language->get("remoteSideModel", "nonEmptyDeleteAsk")), dir.value()),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::None |
                ConfirmDialog::Button::All | ConfirmDialog::Button::Cancel,
            .focusButton = ConfirmDialog::Button::No,
            .onClose = [this,
                           confirmedToDelete = std::move(confirmedToDelete),
                           nonEmpties = std::move(nonEmpties),
                           filesAndEmptyDirs = std::move(filesAndEmptyDirs),
                           currentNonEmpty,
                           dir](std::optional<ConfirmDialog::Button> button) mutable
            {
                if (button && button == ConfirmDialog::Button::Yes)
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

                if (button && button == ConfirmDialog::Button::All)
                {
                    confirmedToDelete.push_back(dir);
                    for (; currentNonEmpty + 1 < nonEmpties.size(); ++currentNonEmpty)
                    {
                        confirmedToDelete.push_back(nonEmpties[currentNonEmpty + 1]);
                    }
                    enqueueDeletes(std::move(confirmedToDelete), std::move(filesAndEmptyDirs));
                    return;
                }

                if (button && button == ConfirmDialog::Button::No)
                {
                    askNonEmptyDirectoryDeletions(
                        std::move(confirmedToDelete),
                        std::move(nonEmpties),
                        std::move(filesAndEmptyDirs),
                        currentNonEmpty + 1
                    );
                    return;
                }

                if (button && button == ConfirmDialog::Button::None)
                {
                    enqueueDeletes(std::move(confirmedToDelete), std::move(filesAndEmptyDirs));
                    return;
                }

                Log::info("User cancelled deletion of non-empty directories.");
            }}
    );
}

void RemoteSideModel::enqueueDeletes(
    std::vector<Paths::RemotePath> nonEmpties,
    std::vector<Paths::RemotePath> filesAndEmptyDirs
)
{
    // Combine into a single bulk-delete request.  File / empty-dir entries
    // become one bulk-delete card; non-empty directories become individual
    // Scan+Delete pairs (their isDirectory=true triggers the recursive flow
    // backend-side).  One RPC for the whole user action.
    std::vector<SharedData::BulkDeleteEntry> entries;
    entries.reserve(filesAndEmptyDirs.size() + nonEmpties.size());
    for (auto& path : filesAndEmptyDirs)
        entries.push_back({.target = std::move(path), .isDirectory = false});
    for (auto& path : nonEmpties)
        entries.push_back({.target = std::move(path), .isDirectory = true});

    if (entries.empty())
        return;

    operationQueue_->enqueueBulkDelete(
        std::move(entries),
        /*insertRefresh*/ true,
        SharedData::OperationMode::Queued,
        /*onBulkComplete*/ {},
        [this](bool success, std::string const& info)
        {
            if (!success)
            {
                Log::error("Bulk delete failed: {}", info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("remoteSideModel", "deleteFailed"),
                    .text = fmt::format(
                        fmt::runtime(language->get("remoteSideModel", "failedToCreateDeleteOperation")), info
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
            }
        }
    );
}

void RemoteSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    using namespace std::string_literals;

    Log::info("Delete items requested: {}", items.size());

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
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("remoteSideModel", "deleteItemsQuestion"),
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items](std::optional<ConfirmDialog::Button> button)
            {
                if (!button || button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Delete items cancelled");
                    return;
                }

                std::vector<Paths::RemotePath> directories;
                std::vector<Paths::RemotePath> files;
                for (auto const& item : items)
                {
                    // A name like "x/.." or ".." must not turn into another delete target.
                    auto path = remotePathOf(item);
                    if (!path)
                    {
                        Log::error("Refusing to delete '{}': not an item of the current directory.", item.name);
                        confirmDialog_->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("remoteSideModel", "deleteFailed"),
                            .text = fmt::format(
                                fmt::runtime(language->get("remoteSideModel", "failedToDeleteItems")),
                                item.name
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                        return;
                    }
                    if (item.type == NuiFileExplorer::Item::Type::Directory)
                        directories.push_back(*std::move(path));
                    else
                        files.push_back(*std::move(path));
                }

                Log::info("Deleting items");
                fileEngine_->remove(
                    std::move(files),
                    std::move(directories),
                    [this](bool success, std::string const& info)
                    {
                        if (!success)
                        {
                            Log::error("Failed to delete files");
                            confirmDialog_->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = language->get("remoteSideModel", "deleteFailed"),
                                .text = fmt::format(
                                    fmt::runtime(language->get("remoteSideModel", "failedToDeleteItems")), info
                                ),
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                            return;
                        }
                        // Refresh list from server:
                        onRefresh();
                    },
                    [this](std::vector<Paths::RemotePath> filesAndEmptyDirs, std::vector<Paths::RemotePath> nonEmpties)
                    {
                        askNonEmptyDirectoryDeletions({}, std::move(nonEmpties), std::move(filesAndEmptyDirs), 0);
                    }
                );
            }}
    );
}

void RemoteSideModel::enqueueSingleDownload(
    SharedData::BulkDownloadEntry const& entry,
    bool allowOverwrite,
    bool insertRefresh
)
{
    operationQueue_->enqueueDownload(
        entry.source,
        entry.destination,
        entry.sizeBytes,
        [this](std::optional<Ids::OperationId> const& opId, std::string const& info)
        {
            if (!opId)
            {
                Log::error("Failed to create download operation: {}", info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("remoteSideModel", "downloadFailed"),
                    .text = fmt::format(
                        fmt::runtime(language->get("remoteSideModel", "failedToCreateDownloadOperation")), info
                    ),
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
    std::vector<SharedData::BulkDownloadEntry> candidates,
    std::shared_ptr<std::vector<bool>> existsResults,
    std::size_t index,
    bool overwriteNever,
    bool overwriteAlways,
    std::shared_ptr<std::vector<SharedData::BulkDownloadEntry>> accepted
)
{
    // Lazily allocate the accumulator on the first (top-level) call.  All
    // recursive calls below thread the same shared_ptr so per-file
    // confirmations land in one batch that's flushed as a single bulk RPC at
    // the terminal recursion.
    if (!accepted)
        accepted = std::make_shared<std::vector<SharedData::BulkDownloadEntry>>();

    // Every entry in `accepted` is either a non-existing destination or an
    // item the user explicitly approved for overwrite (Yes / All); the "No"
    // and "None" branches skip the push.  So allowOverwrite=true at flush
    // time is semantically correct regardless of the overwriteAlways flag.
    auto flushAccepted = [this, &accepted]()
    {
        if (accepted->empty())
            return;
        // Single-file fast path: a one-entry flush skips the bulk machinery
        // so the operation appears as a regular transfer card (progress bar
        // + resumable) rather than a bulk aggregate.  Directories stay on
        // the bulk path because they need the Scan+Bulk pair.
        if (accepted->size() == 1 && !accepted->front().isDirectory)
        {
            enqueueSingleDownload(
                accepted->front(),
                /*allowOverwrite=*/true,
                /*insertRefresh=*/true
            );
            accepted->clear();
            return;
        }
        // Terminal: flush the accumulated batch in a single RPC. The backend
        // skips per-file lstats for file entries (sizes are trusted) and
        // performs all queueing inside one strand dispatch — replacing N
        // RPCs with one.
        operationQueue_->enqueueBulkDownload(
            std::move(*accepted),
            /*allowOverwrite*/ true,
            /*insertRefresh*/ true,
            SharedData::OperationMode::Queued,
            /*onEachComplete*/ {},
            [this](bool success, std::string const& info)
            {
                if (!success)
                {
                    Log::error("Bulk download failed: {}", info);
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = language->get("remoteSideModel", "downloadFailed"),
                        .text = fmt::format(
                            fmt::runtime(language->get("remoteSideModel", "failedToCreateDownloadOperation")), info
                        ),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                }
            }
        );
    };

    if (index == candidates.size())
    {
        flushAccepted();
        return;
    }

    if (overwriteAlways)
    {
        for (; index < candidates.size(); ++index)
            accepted->push_back(approvedForOverwrite(candidates[index]));
        flushAccepted();
        return;
    }

    // Iterate synchronously until we hit either end-of-list or a conflict
    // that requires an async confirmation dialog.  Recursing per-item would
    // blow WASM's ~64KB stack at ~175 deep for a 1000-file batch, since each
    // frame holds a moved copy of the candidates.
    while (index < candidates.size())
    {
        auto const& candidate = candidates[index];
        // existsResults was filled by a single batched RpcFilesystem::existsBatch
        // call before the loop started, so this adds zero RPC round-trips.
        // Defensive: if results are missing or shorter than expected, treat
        // the file as not-existing.
        // Without an answer the destination counts as existing, so the user is asked.
        const bool exists = !existsResults || index >= existsResults->size() || (*existsResults)[index];

        if (!exists)
        {
            accepted->push_back(candidate);
            ++index;
            continue;
        }
        if (exists && overwriteNever)
        {
            ++index;
            continue;
        }

        // Conflict that actually needs a user decision — break out to the
        // async prompt below.  The dialog's onClose callback resumes the
        // iteration at index+1 via one non-tail recursion, keeping the
        // number of stacked frames bounded by the number of conflicts (not
        // the number of files).
        break;
    }

    if (index >= candidates.size())
    {
        // All items processed without needing further prompts — flush.
        flushAccepted();
        return;
    }

    auto const& candidate = candidates[index];
    Log::info("Downloading '{}' to '{}'.", candidate.source.value(), candidate.destination.toUtf8());

    {
        Log::info("File already exists: {}", candidate.destination.toUtf8());
        confirmDialog_->open(
            {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
                .headerText = language->get("remoteSideModel", "fileAlreadyExistsOverwrite"),
                .text = language->get("remoteSideModel", "allowOverwritingFile"),
                .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                    ConfirmDialog::Button::None | ConfirmDialog::Button::Cancel,
                .focusButton = ConfirmDialog::Button::No,
                .listItems = {{.text = candidate.destination.toUtf8(), .description = language->get("fileExplorer", "fileAlreadyExists")}},
                .onClose = [this,
                               candidates = std::move(candidates),
                               existsResults,
                               index,
                               overwriteNever,
                               overwriteAlways,
                               accepted](std::optional<ConfirmDialog::Button> button) mutable
                {
                    if (button && button == ConfirmDialog::Button::Yes)
                    {
                        accepted->push_back(approvedForOverwrite(candidates[index]));
                        downloadItemsConfirmed(
                            std::move(candidates),
                            std::move(existsResults),
                            index + 1,
                            overwriteNever,
                            overwriteAlways,
                            std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::No)
                    {
                        Log::info(
                            "Skipping download of existing file: {}", candidates[index].destination.toUtf8()
                        );
                        downloadItemsConfirmed(
                            std::move(candidates),
                            std::move(existsResults),
                            index + 1,
                            overwriteNever,
                            overwriteAlways,
                            std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::All)
                    {
                        Log::info("Overwriting all existing files from now on.");
                        accepted->push_back(approvedForOverwrite(candidates[index]));
                        downloadItemsConfirmed(
                            std::move(candidates),
                            std::move(existsResults),
                            index + 1,
                            overwriteNever,
                            /*overwriteAlways*/ true,
                            std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::None)
                    {
                        Log::info("Skipping all existing files from now on.");
                        downloadItemsConfirmed(
                            std::move(candidates),
                            std::move(existsResults),
                            index + 1,
                            /*overwriteNever*/ true,
                            overwriteAlways,
                            std::move(accepted)
                        );
                    }
                    else
                    {
                        // Cancel — flush whatever the user has already
                        // accepted so far and stop iterating.
                        const auto terminalIndex = candidates.size();
                        downloadItemsConfirmed(
                            std::move(candidates),
                            std::move(existsResults),
                            terminalIndex,
                            overwriteNever,
                            overwriteAlways,
                            std::move(accepted)
                        );
                    }
                }}
        );
    }
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

    if (items.empty())
    {
        Log::error("No items selected for download");
        return;
    }

    if (!localModel_)
    {
        Log::error("Cannot download items: local model is not set");
        return;
    }

    const auto refuse = [this](Paths::PathError const& error)
    {
        showPathRefused(language->get("remoteSideModel", "downloadFailed"), error);
    };
    const auto destinationDirectory = localModel_->currentLocalPath().and_then(
        [&subDir](Paths::LocalPath const& directory) -> std::expected<Paths::LocalPath, Paths::PathError>
        {
            if (!subDir)
                return directory;
            return Paths::LocalPathSegment::make(*subDir).transform(
                [&directory](Paths::LocalPathSegment const& segment)
                {
                    return directory / segment;
                }
            );
        }
    );
    if (!destinationDirectory)
        return refuse(destinationDirectory.error());

    std::vector<SharedData::BulkDownloadEntry> candidates;
    candidates.reserve(items.size());
    for (auto const& item : items)
    {
        // A name this platform cannot hold is refused before anything is written.
        auto source = remotePathOf(item);
        auto destination = source.and_then(
            [&destinationDirectory](Paths::RemotePath const& path)
            {
                return Paths::LocalPathSegment::make(*path.filename())
                    .transform(
                        [&destinationDirectory](Paths::LocalPathSegment const& segment)
                        {
                            return *destinationDirectory / segment;
                        }
                    );
            }
        );
        if (!destination)
            return refuse(destination.error());
        candidates.push_back(
            SharedData::BulkDownloadEntry{
                .source = *std::move(source),
                .destination = *std::move(destination),
                .sizeBytes = item.size,
                .isDirectory = item.isDirectory(),
                .mtime = item.mtime,
                .mtimeNsec = item.mtimeNsec,
            }
        );
    }

    const auto itemsSize = candidates.size();
    std::string confirmText;
    if (itemsSize > 1)
    {
        confirmText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "downloadConfirmIfMultiple")),
            itemsSize,
            destinationDirectory->toUtf8()
        );
    }
    else
    {
        confirmText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "downloadConfirmIfSingle")),
            candidates.front().source.value(),
            destinationDirectory->toUtf8()
        );
    }

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& candidate : candidates)
        listItems.push_back({candidate.source.value(), ""});

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("remoteSideModel", "downloadItemsQuestion"),
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, candidates = std::move(candidates)](std::optional<ConfirmDialog::Button> button) mutable
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Download items cancelled");
                    return;
                }

                Log::info("Downloading items.");

                // Batched existence probe: one RPC for the whole batch instead of N. The recursion below then walks
                // the cached result vector with zero further round-trips.
                std::vector<std::string> destinations;
                destinations.reserve(candidates.size());
                for (auto const& candidate : candidates)
                    destinations.push_back(candidate.destination.toUtf8());

                Nui::RpcClient::callWithBackChannel(
                    "RpcFilesystem::existsBatch",
                    [this, candidates = std::move(candidates)](Nui::val response) mutable
                    {
                        // An unknown destination is never taken for absent, the batch overwrites what it accepts.
                        if (!response.hasOwnProperty("success") || !response["success"].as<bool>() ||
                            !response.hasOwnProperty("exists"))
                        {
                            return showExistenceCheckFailed(
                                response.hasOwnProperty("error") ? language->translate(response["error"].as<std::string>())
                                                                 : std::string{"RpcFilesystem::existsBatch"}
                            );
                        }

                        auto existsResults = std::make_shared<std::vector<bool>>();
                        auto arr = response["exists"];
                        const auto length = arr["length"].as<long long>();
                        existsResults->reserve(static_cast<std::size_t>(length));
                        for (long long idx = 0; idx < length; ++idx)
                            existsResults->push_back(arr[static_cast<int>(idx)].as<bool>());
                        if (existsResults->size() != candidates.size())
                        {
                            return showExistenceCheckFailed(
                                fmt::format("{} of {} answers", existsResults->size(), candidates.size())
                            );
                        }

                        downloadItemsConfirmed(std::move(candidates), std::move(existsResults));
                    },
                    destinations
                );
            }}
    );
}

namespace
{
    /**
     * @brief Translate the frontend-local ArchiveCodec to the integer value the
     *        backend expects (the raw int of TarArchive::Compression). The order
     *        must match tar_archive/compression.hpp: Auto=0, None=1, Gzip=2,
     *        Bzip2=3, Zstd=4, Xz=5.
     */
    int backendCompressionCode(ArchiveCodec codec)
    {
        switch (codec)
        {
            case ArchiveCodec::None:
                return 1;
            case ArchiveCodec::Gzip:
                return 2;
            case ArchiveCodec::Bzip2:
                return 3;
            case ArchiveCodec::Zstd:
                return 4;
            case ArchiveCodec::Xz:
                return 5;
        }
        return 2;
    }
}

void RemoteSideModel::onTransferAsArchive(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    if (items.empty())
    {
        Log::error("No items selected for archive download");
        return;
    }
    if (archiveTransferDialog_ == nullptr)
    {
        Log::error("ArchiveTransferDialog is not wired; cannot open archive download dialog");
        return;
    }
    if (!localModel_)
    {
        Log::error("Cannot download as archive: local model is not wired");
        return;
    }

    // Root entries: files go straight in, directories are flattened on the
    // backend via a recursive SFTP listDirectory in the archive op's
    // prepareInStrand step. Symlinks and other non-regular / non-directory
    // types are logged and dropped there too.
    std::vector<SharedData::ArchiveDownloadRoot> roots;
    roots.reserve(items.size());
    for (auto const& item : items)
    {
        auto path = remotePathOf(item);
        if (!path)
            return showPathRefused(language->get("remoteSideModel", "archiveDownloadFailed"), path.error());
        roots.push_back(
            SharedData::ArchiveDownloadRoot{
                .path = *std::move(path),
                .entry = static_cast<SharedData::DirectoryEntry const&>(item),
            }
        );
    }

    const auto defaultStem =
        items.size() == 1 ? items.front().path.filename().generic_string() : language->get("fileExplorer", "archiveDefaultName");

    archiveTransferDialog_->open({
        .headerText = language->get("fileExplorer", "contextMenu", "downloadAsArchive"),
        .initialFileStem = defaultStem,
        .initialCodec = ArchiveCodec::Gzip,
        .initialCompressionLevel = 5,
        .onConfirm =
            [this, roots = std::move(roots)](std::optional<ArchiveTransferResult> const& result) mutable
        {
            if (!result)
            {
                Log::info("Download as Archive: user cancelled");
                return;
            }
            if (!localModel_)
                return;

            const std::string filename = result->fileStem + ".tar" + archiveCodecExtension(result->codec);
            auto archivePath = localModel_->currentLocalPath().and_then(
                [&filename](Paths::LocalPath const& directory)
                {
                    return Paths::LocalPathSegment::make(filename).transform(
                        [&directory](Paths::LocalPathSegment const& segment)
                        {
                            return directory / segment;
                        }
                    );
                }
            );
            if (!archivePath)
                return showPathRefused(language->get("remoteSideModel", "archiveDownloadFailed"), archivePath.error());
            const auto localPath = *std::move(archivePath);

            operationQueue_->enqueueArchiveDownload(
                std::move(roots),
                localPath,
                backendCompressionCode(result->codec),
                result->compressionLevel,
                /*mayOverwrite=*/false,
                SharedData::OperationMode::Queued,
                [this, localPath](std::optional<Ids::OperationId> const& opId, std::string const& info)
                {
                    if (!opId)
                    {
                        Log::error("Archive download failed to enqueue: {}", info);
                        confirmDialog_->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("remoteSideModel", "archiveDownloadFailed"),
                            .text = info,
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                        return;
                    }
                    Log::info("Archive download queued as {} (destination {})", opId->value(), localPath.toUtf8());
                }
            );
        },
    });
}

void RemoteSideModel::renameItem(Paths::RemotePath const& from, Paths::RemotePath const& to, bool overwrite)
{
    fileEngine_->rename(
        from,
        to,
        overwrite,
        [this, from, to, overwrite](bool success, std::string const& info)
        {
            if (!success)
            {
                if (info == "backend.rename.targetExists" && !overwrite)
                {
                    confirmReplaceOnRename(
                        std::string{to.filename()->view()},
                        [this, from, to]()
                        {
                            renameItem(from, to, true);
                        }
                    );
                    return;
                }
                Log::error("Failed to rename item '{}' to '{}': {}", from.value(), to.value(), info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "",
                    .text = fmt::format(
                        fmt::runtime(language->get("remoteSideModel", "failedToRenameItem")),
                        from.value(),
                        to.value(),
                        language->translate(info)
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }
            Log::info("Renamed item '{}' to '{}'", from.value(), to.value());
            onRefresh();
        }
    );
}

void RemoteSideModel::onRename(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Rename item requested: {}", item.path.generic_string());

    // The item's own path: the explorer may already show another directory.
    auto from = remotePathOf(item);
    if (!from)
        return showPathRefused(language->get("remoteSideModel", "rename"), from.error());

    inputDialog_->open({
        .whatFor = language->get("remoteSideModel", "rename"),
        .prompt =
            fmt::format(fmt::runtime(language->get("remoteSideModel", "renamePrompt")), item.path.filename().string()),
        .headerText = fmt::format(
            fmt::runtime(language->get("remoteSideModel", "renameWithItem")), item.path.filename().string()
        ),
        .initialValue = item.path.filename().string(),
        .isPassword = false,
        .onConfirm = [this, from = *std::move(from), item](std::optional<std::string> const& name)
        {
            if (!name || name->empty())
                return;
            if (*name == item.path.filename().string())
                return;
            if (!isValidItemName(*name))
                return showInvalidItemName(*name);
            const auto segment = Paths::PathSegment::make(*name);
            const auto directory = from.parent();
            if (!segment || !directory)
                return showInvalidItemName(*name);

            Log::info("Renaming item to: {}", *name);
            renameItem(from, *directory / *segment, false);
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
        currentPath_.value(),
        [this, directory = currentPath_.value()](
            std::optional<std::vector<SharedData::DirectoryEntry>> const& entries, std::string const& info
        )
        {
            onDirectoryListing(directory, entries, info);
        }
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

void RemoteSideModel::onDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith)
{
    CHECK_COMPLETE();

    if (fileTracking_ == nullptr)
    {
        Log::error("File tracking panel is not available, cannot download/open file.");
        return;
    }

    Nui::RpcClient::callWithBackChannel(
        "FileTracking::createInstance",
        [this, item, openWith](Nui::val response)
        {
            if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
            {
                Log::error("FileTracking: createInstance failed");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("fileTrackingPanel", "instanceCreationFailed"),
                    .text = language->get("fileTrackingPanel", "instanceCreationFailedDesc"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }
            onFileTrackingInstanceCreated(
                item,
                openWith,
                false,
                Ids::makeInstanceId(response["instanceId"].as<std::string>()),
                response["instanceDir"].as<std::string>()
            );
        }
    );
}

void RemoteSideModel::onWatchDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith)
{
    CHECK_COMPLETE();

    if (fileTracking_ == nullptr)
    {
        Log::error("File tracking panel is not available, cannot watch/download/open file.");
        return;
    }

    // First create an instance where stuff to watch is placed:
    Nui::RpcClient::callWithBackChannel(
        "FileTracking::createInstance",
        [this, item, openWith](Nui::val response)
        {
            if (!response.hasOwnProperty("success") || !response["success"].as<bool>())
            {
                Log::error("FileTracking: createInstance failed");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("fileTrackingPanel", "instanceCreationFailed"),
                    .text = language->get("fileTrackingPanel", "instanceCreationFailedDesc"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }
            onFileTrackingInstanceCreated(
                item,
                openWith,
                true,
                Ids::makeInstanceId(response["instanceId"].as<std::string>()),
                response["instanceDir"].as<std::string>()
            );
        }
    );
}

void RemoteSideModel::onFileTrackingInstanceCreated(
    NuiFileExplorer::Item const& item,
    bool openWith,
    bool synchronize,
    Ids::InstanceId const& instanceId,
    std::string const& instanceDirStr
)
{
    Log::info("FileTracking: createInstance succeeded, id={}, dir={}", instanceId.value(), instanceDirStr);

    const auto instanceDirectory = Paths::LocalPath::fromUtf8(instanceDirStr);
    const auto remotePath = remotePathOf(item);
    const auto localPath = remotePath.and_then(
        [&instanceDirectory](Paths::RemotePath const& path)
        {
            return Paths::LocalPathSegment::make(*path.filename())
                .and_then(
                    [&instanceDirectory](Paths::LocalPathSegment const& segment)
                    {
                        return instanceDirectory.transform(
                            [&segment](Paths::LocalPath const& directory)
                            {
                                return directory / segment;
                            }
                        );
                    }
                );
        }
    );
    if (!localPath)
        return showPathRefused(language->get("remoteSideModel", "downloadFailed"), localPath.error());

    operationQueue_->enqueueDownload(
        *remotePath,
        *localPath,
        item.size,
        [this,
            instanceId,
            instanceDirectory = *instanceDirectory,
            remotePath = *remotePath,
            localPath = *localPath,
            openWith,
            synchronize](
            std::optional<Ids::OperationId> const& opId, std::string const& info
        )
        {
            if (!opId)
            {
                Log::error("FileTracking: failed to enqueue download for instance {}: {}", instanceId.value(), info);
                return;
            }
            operationQueue_->addCompletionCallback(
                *opId,
                [this, instanceId, instanceDirectory, remotePath, localPath, openWith, synchronize](bool success)
                {
                    onFileDownloadComplete(
                        instanceId, instanceDirectory, remotePath, localPath, openWith, synchronize, success
                    );
                }
            );
        },
        false, // allowOverwrite
        false, // insertRefresh
        false, // createMissingDirectories
        SharedData::OperationMode::PriorityQueued
    );
}

void RemoteSideModel::onFileDownloadComplete(
    Ids::InstanceId const& instanceId,
    Paths::LocalPath const& instanceDirectory,
    Paths::RemotePath const& remotePath,
    Paths::LocalPath const& localPath,
    bool openWith,
    bool synchronize,
    bool success
)
{
    if (!success)
    {
        Log::error("FileTracking: download failed for {}", localPath.toUtf8());
        return;
    }
    Log::info("FileTracking: download completed for {}", localPath.toUtf8());

    if (!synchronize)
    {
        Log::info("FileTracking: opening {} (openWith={}, no sync)", localPath.toUtf8(), openWith);
        FileExplorer::openLocalFile(*confirmDialog_, localPath, openWith);
        return;
    }

    Nui::RpcClient::callWithBackChannel(
        "FileTracking::addWatch",
        [this, instanceId, instanceDirectory, remotePath, localPath, openWith](Nui::val watchResponse)
        {
            if (!watchResponse.hasOwnProperty("success") || !watchResponse["success"].as<bool>())
            {
                Log::error("FileTracking: addWatch failed for instance {}", instanceId.value());
                return;
            }
            Log::info("FileTracking: addWatch succeeded for instance {}", instanceId.value());
            onFileWatchAdded(instanceId, instanceDirectory, remotePath, localPath, openWith);
        },
        instanceId.value(),
        instanceDirectory.toUtf8(),
        true,
        fileTracking_ ? fileTracking_->downloadTempSuffix() : std::string{Persistence::defaultTempFileSuffix}
    );
}

void RemoteSideModel::onFileWatchAdded(
    Ids::InstanceId const& instanceId,
    Paths::LocalPath const& instanceDirectory,
    Paths::RemotePath const& remotePath,
    Paths::LocalPath const& localPath,
    bool openWith
)
{
    fileTracking_->startWatching(instanceId, instanceDirectory, remotePath, localPath);

    Log::info("FileTracking: opening {} (openWith={})", localPath.toUtf8(), openWith);

    FileExplorer::openLocalFile(*confirmDialog_, localPath, openWith);
}

// --- IPlacesProvider ---

void RemoteSideModel::setRemoteHome(std::filesystem::path home, bool readUserDirectories)
{
    remoteHome_ = std::move(home);
    readUserDirectories_ = readUserDirectories;
    remotePlaces_.reset();
}

void RemoteSideModel::requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback)
{
    if (remoteHome_.empty())
        return callback({});
    if (remotePlaces_)
        return callback(defaultPlaceEntries());

    pendingPlacesRequests_.push_back(std::move(callback));
    if (pendingPlacesRequests_.size() > 1)
        return;
    if (!readUserDirectories_)
        return placesFromHomeListing();

    fileEngine_->userDirectories(
        remoteHome_,
        [this, home = remoteHome_](
            std::optional<std::vector<std::pair<std::string, std::filesystem::path>>> const& directories,
            std::string const&
        )
        {
            if (home != remoteHome_)
                return retryPlacesRequests();
            if (!directories || directories->empty())
                return placesFromHomeListing();

            remotePlaces_.emplace();
            for (auto const& place : Utility::defaultPlaces)
            {
                if (place.userDirectoryName.empty())
                    continue;
                const auto directory = std::ranges::find(
                    *directories, place.userDirectoryName, &std::pair<std::string, std::filesystem::path>::first
                );
                if (directory != directories->end())
                    remotePlaces_->emplace_back(std::string{place.kind}, directory->second);
            }
            finishPlacesRequests();
        }
    );
}

void RemoteSideModel::placesFromHomeListing()
{
    fileEngine_->listDirectory(
        remoteHome_,
        [this, home = remoteHome_](
            std::optional<std::vector<SharedData::DirectoryEntry>> const& entries, std::string const& info
        )
        {
            if (home != remoteHome_)
                return retryPlacesRequests();
            if (!entries)
            {
                Log::warn("Failed to list the remote home directory for the default places: {}", info);
                return finishPlacesRequests();
            }

            remotePlaces_.emplace();
            for (auto const& place : Utility::defaultPlaces)
            {
                const auto exists = std::ranges::any_of(
                    *entries,
                    [&place](SharedData::DirectoryEntry const& entry)
                    {
                        const bool isDirectory = entry.isDirectory() ||
                            (entry.resolvedTarget != nullptr && entry.resolvedTarget->isDirectory());
                        return isDirectory && entry.path.filename().generic_string() == place.directoryName;
                    }
                );
                if (exists)
                    remotePlaces_->emplace_back(std::string{place.kind}, remoteHome_ / place.directoryName);
            }
            finishPlacesRequests();
        }
    );
}

void RemoteSideModel::finishPlacesRequests()
{
    const auto entries = defaultPlaceEntries();
    for (auto const& pending : std::exchange(pendingPlacesRequests_, {}))
        pending(entries);
}

void RemoteSideModel::retryPlacesRequests()
{
    for (auto& pending : std::exchange(pendingPlacesRequests_, {}))
        requestDefaultPlaces(std::move(pending));
}

std::vector<RemoteSideModel::PlaceEntry> RemoteSideModel::defaultPlaceEntries() const
{
    std::vector<PlaceEntry> entries{
        {.icon = iconForPlaceKind("home"),
            .name = placeDisplayName("home", "home"),
            .path = remoteHome_.generic_string()}
    };
    if (!remotePlaces_)
        return entries;

    for (auto const& [kind, path] : *remotePlaces_)
    {
        entries.push_back(
            {.icon = iconForPlaceKind(kind), .name = placeDisplayName(kind, kind), .path = path.generic_string()}
        );
    }
    return entries;
}