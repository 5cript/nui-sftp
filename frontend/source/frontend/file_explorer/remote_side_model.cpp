#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/session_components/file_tracking.hpp>

#include <ui5-sap-icons/icons/home.hpp>
#include <ui5-sap-icons/icons/desktop-mobile.hpp>
#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/documents.hpp>
#include <ui5-sap-icons/icons/picture.hpp>
#include <ui5-sap-icons/icons/video.hpp>
#include <ui5-sap-icons/icons/folder.hpp>
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
#include <nui-file-explorer/preprocessor.hpp>
#include <script-nui-components/popup_menu.hpp>
#include <log/log.hpp>
#include <ids/ids.hpp>
#include <shared_data/file_operations/operation_mode.hpp>

#include <nui/rpc.hpp>

#include <algorithm>
#include <iterator>

using namespace std::string_literals;

namespace
{
    Nui::ElementRenderer iconForPlaceName(std::string const& name)
    {
        if (name == "Home")
            return Ui5Icons::home();
        if (name == "Desktop")
            return Ui5Icons::desktop_mobile();
        if (name == "Downloads")
            return Ui5Icons::download();
        if (name == "Documents")
            return Ui5Icons::documents();
        if (name == "Pictures")
            return Ui5Icons::picture();
        if (name == "Videos")
            return Ui5Icons::video();
        return Ui5Icons::folder();
    }
}

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

    if (singleItem && selectedItems.front().isDirectoryLike() && selectedItems.front().path.filename() != "..")
    {
        const auto& selPath = selectedItems.front().fullPath;
        const auto& favs = favorites_->value();
        const bool isFav = std::find(favs.begin(), favs.end(), selPath) != favs.end();
        items.push_back(
            Snc::PopupMenu::item(
                isFav ? "Remove from Favorites" : "Add to Favorites",
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
            "Download as Archive",
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

void RemoteSideModel::setLocalModel(SideModel* model)
{
    localModel_ = model;
}

void RemoteSideModel::setFileTracking(FileTrackingPanel* fileTracking)
{
    fileTracking_ = fileTracking;
}

bool RemoteSideModel::isComplete() const
{
    return localModel_ != nullptr && SideModel::isComplete();
}

void RemoteSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    if (!item.isDirectoryLike())
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

                Log::info("Creating new file: {}", *name);
                if (name->find('/') != std::string::npos)
                {
                    Log::error("Invalid file name (cannot contain slashes): {}", *name);
                    return;
                }
                fileEngine_->createFile(
                    currentPath_.value() / *name,
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

void RemoteSideModel::onError(std::string const& error)
{
    Log::error("File grid error (remote side): {}", error);
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
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
        {.styleVariant = ScriptNuiComponents::StyleVariant::Warning,
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
    std::vector<std::filesystem::path> nonEmpties,
    std::vector<std::filesystem::path> filesAndEmptyDirs
)
{
    // Combine into a single bulk-delete request.  File / empty-dir entries
    // become one bulk-delete card; non-empty directories become individual
    // Scan+Delete pairs (their isDirectory=true triggers the recursive flow
    // backend-side).  One RPC for the whole user action.
    std::vector<SharedData::BulkAddEntry> entries;
    entries.reserve(filesAndEmptyDirs.size() + nonEmpties.size());
    for (auto const& path : filesAndEmptyDirs)
    {
        entries.push_back(SharedData::BulkAddEntry{
            .src = path,
            .dst = {},
            .sizeBytes = 0,
            .isDirectory = false,
        });
    }
    for (auto const& path : nonEmpties)
    {
        entries.push_back(SharedData::BulkAddEntry{
            .src = path,
            .dst = {},
            .sizeBytes = 0,
            .isDirectory = true,
        });
    }

    if (entries.empty())
        return;

    operationQueue_->enqueueBulkDelete(
        std::move(entries),
        /*insertRefresh*/ true,
        SharedData::OperationMode::Queued,
        /*onBulkComplete*/ {},
        [this](bool success, std::string const& info) {
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
    std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> downloadItems,
    std::shared_ptr<std::vector<bool>> existsResults,
    std::size_t index,
    bool overwriteNever,
    bool overwriteAlways,
    std::shared_ptr<std::vector<SharedData::BulkAddEntry>> accepted
)
{
    // Lazily allocate the accumulator on the first (top-level) call.  All
    // recursive calls below thread the same shared_ptr so per-file
    // confirmations land in one batch that's flushed as a single bulk RPC at
    // the terminal recursion.
    if (!accepted)
        accepted = std::make_shared<std::vector<SharedData::BulkAddEntry>>();

    auto pushEntry = [](
        std::vector<SharedData::BulkAddEntry>& bucket,
        NuiFileExplorer::Item const& remoteItem,
        NuiFileExplorer::Item const& localItem
    ) {
        bucket.push_back(SharedData::BulkAddEntry{
            .src = !remoteItem.fullPath.empty() ? remoteItem.fullPath : remoteItem.path,
            .dst = !localItem.fullPath.empty() ? localItem.fullPath : localItem.path,
            .sizeBytes = remoteItem.size,
            .isDirectory = remoteItem.isDirectory(),
            .mtime = remoteItem.mtime,
            .mtimeNsec = remoteItem.mtimeNsec,
        });
    };

    // Every entry in `accepted` is either a non-existing destination or an
    // item the user explicitly approved for overwrite (Yes / All); the "No"
    // and "None" branches skip the push.  So allowOverwrite=true at flush
    // time is semantically correct regardless of the overwriteAlways flag.
    auto flushAccepted = [this, &accepted]() {
        if (accepted->empty())
            return;
        // Single-file fast path: a one-entry flush skips the bulk machinery
        // so the operation appears as a regular transfer card (progress bar
        // + resumable) rather than a bulk aggregate.  Directories stay on
        // the bulk path because they need the Scan+Bulk pair.
        if (accepted->size() == 1 && !accepted->front().isDirectory)
        {
            auto const& entry = accepted->front();
            SharedData::DirectoryEntry remoteEntry{};
            remoteEntry.path = entry.src;
            remoteEntry.size = entry.sizeBytes;
            remoteEntry.type = SharedData::FileType::Regular;
            remoteEntry.mtime = entry.mtime;
            remoteEntry.mtimeNsec = entry.mtimeNsec;
            SharedData::DirectoryEntry localEntry{};
            localEntry.path = entry.dst;
            localEntry.type = SharedData::FileType::Regular;
            enqueueSingleDownload(
                NuiFileExplorer::Item{remoteEntry},
                NuiFileExplorer::Item{localEntry},
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
            [this](bool success, std::string const& info) {
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

    if (index == downloadItems.size())
    {
        flushAccepted();
        return;
    }

    if (overwriteAlways)
    {
        for (; index < downloadItems.size(); ++index)
            pushEntry(*accepted, downloadItems[index].first, downloadItems[index].second);
        flushAccepted();
        return;
    }

    // Iterate synchronously until we hit either end-of-list or a conflict
    // that requires an async confirmation dialog.  Recursing per-item would
    // blow WASM's ~64KB stack at ~175 deep for a 1000-file batch, since each
    // frame holds a moved copy of downloadItems.
    while (index < downloadItems.size())
    {
        auto const& item = downloadItems[index];
        // existsResults was filled by a single batched RpcFilesystem::existsBatch
        // call before the loop started, so this adds zero RPC round-trips.
        // Defensive: if results are missing or shorter than expected, treat
        // the file as not-existing.
        const bool exists = (existsResults && index < existsResults->size()) ? (*existsResults)[index] : false;

        if (!exists)
        {
            pushEntry(*accepted, item.first, item.second);
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

    if (index >= downloadItems.size())
    {
        // All items processed without needing further prompts — flush.
        flushAccepted();
        return;
    }

    auto const& item = downloadItems[index];
    Log::info("Downloading '{}' to '{}'.", item.first.path.generic_string(), item.second.path.generic_string());

    {
        Log::info("File already exists: {}", item.second.path.generic_string());
        confirmDialog_->open(
            {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
                .headerText = language->get("remoteSideModel", "fileAlreadyExistsOverwrite"),
                .text = language->get("remoteSideModel", "allowOverwritingFile"),
                .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                    ConfirmDialog::Button::None | ConfirmDialog::Button::Cancel,
                .focusButton = ConfirmDialog::Button::No,
                .listItems = {{.text = item.second.path.generic_string(), .description = "File already exists"}},
                .onClose = [this,
                               downloadItems = std::move(downloadItems),
                               existsResults,
                               index,
                               overwriteNever,
                               overwriteAlways,
                               accepted,
                               pushEntry](std::optional<ConfirmDialog::Button> button) mutable
                {
                    if (button && button == ConfirmDialog::Button::Yes)
                    {
                        pushEntry(*accepted, downloadItems[index].first, downloadItems[index].second);
                        downloadItemsConfirmed(
                            std::move(downloadItems), std::move(existsResults), index + 1,
                            overwriteNever, overwriteAlways, std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::No)
                    {
                        Log::info(
                            "Skipping download of existing file: {}",
                            downloadItems[index].second.path.generic_string()
                        );
                        downloadItemsConfirmed(
                            std::move(downloadItems), std::move(existsResults), index + 1,
                            overwriteNever, overwriteAlways, std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::All)
                    {
                        Log::info("Overwriting all existing files from now on.");
                        pushEntry(*accepted, downloadItems[index].first, downloadItems[index].second);
                        downloadItemsConfirmed(
                            std::move(downloadItems), std::move(existsResults), index + 1,
                            overwriteNever, /*overwriteAlways*/ true, std::move(accepted)
                        );
                    }
                    else if (button && button == ConfirmDialog::Button::None)
                    {
                        Log::info("Skipping all existing files from now on.");
                        downloadItemsConfirmed(
                            std::move(downloadItems), std::move(existsResults), index + 1,
                            /*overwriteNever*/ true, overwriteAlways, std::move(accepted)
                        );
                    }
                    else
                    {
                        // Cancel — flush whatever the user has already
                        // accepted so far and stop iterating.
                        const auto terminalIndex = downloadItems.size();
                        downloadItemsConfirmed(
                            std::move(downloadItems), std::move(existsResults), terminalIndex,
                            overwriteNever, overwriteAlways, std::move(accepted)
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
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = "Download Items?",
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items, destinationDir](std::optional<ConfirmDialog::Button> button)
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
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
                        localItem.fullPath = localItem.path;
                        remoteItem.path = currentPath_.value() / item.path;
                        remoteItem.fullPath = remoteItem.path;
                        return std::make_pair(remoteItem, localItem);
                    }
                );

                Log::info("Downloading items.");

                // Batched existence probe: one RPC for the whole batch
                // instead of N.  The recursion below then walks the cached
                // result vector with zero further round-trips.
                std::vector<std::string> destPathStrings;
                destPathStrings.reserve(downloadItems.size());
                for (auto const& pair : downloadItems)
                    destPathStrings.push_back(pair.second.path.generic_string());

                Nui::RpcClient::callWithBackChannel(
                    "RpcFilesystem::existsBatch",
                    [this, downloadItems = std::move(downloadItems)](Nui::val response) mutable
                    {
                        auto existsResults = std::make_shared<std::vector<bool>>();

                        if (!response.hasOwnProperty("success") || !response["success"].as<bool>() ||
                            !response.hasOwnProperty("exists"))
                        {
                            Log::warn(
                                "RpcFilesystem::existsBatch failed; assuming nothing exists yet"
                            );
                            existsResults->assign(downloadItems.size(), false);
                        }
                        else
                        {
                            auto arr = response["exists"];
                            const auto length = arr["length"].as<long long>();
                            existsResults->reserve(static_cast<std::size_t>(length));
                            for (long long idx = 0; idx < length; ++idx)
                                existsResults->push_back(arr[static_cast<int>(idx)].as<bool>());
                            // Defensive padding in case the backend returned
                            // fewer entries than requested.
                            while (existsResults->size() < downloadItems.size())
                                existsResults->push_back(false);
                        }

                        downloadItemsConfirmed(std::move(downloadItems), std::move(existsResults));
                    },
                    destPathStrings
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
            case ArchiveCodec::None:  return 1;
            case ArchiveCodec::Gzip:  return 2;
            case ArchiveCodec::Bzip2: return 3;
            case ArchiveCodec::Zstd:  return 4;
            case ArchiveCodec::Xz:    return 5;
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
    if (!localModel_->currentPath().value().empty() == false)
    {
        Log::error("Local side has no active path; cannot pick a destination");
        return;
    }

    // Root entries: files go straight in, directories are flattened on the
    // backend via a recursive SFTP listDirectory in the archive op's
    // prepareInStrand step. Symlinks and other non-regular / non-directory
    // types are logged and dropped there too.
    std::vector<SharedData::DirectoryEntry> rootEntries;
    rootEntries.reserve(items.size());
    for (auto const& item : items)
    {
        SharedData::DirectoryEntry entry = item;
        entry.fullPath = currentPath_.value() / item.path;
        rootEntries.push_back(std::move(entry));
    }

    if (rootEntries.empty())
        return;

    const auto defaultStem = items.size() == 1
        ? items.front().path.filename().generic_string()
        : std::string{"archive"};

    archiveTransferDialog_->open({
        .headerText = "Download as Archive",
        .initialFileStem = defaultStem,
        .initialCodec = ArchiveCodec::Gzip,
        .initialCompressionLevel = 5,
        .onConfirm = [this, entries = std::move(rootEntries)](
                         std::optional<ArchiveTransferResult> const& result
                     ) mutable
        {
            if (!result)
            {
                Log::info("Download as Archive: user cancelled");
                return;
            }
            if (!localModel_)
                return;

            const std::string filename =
                result->fileStem + ".tar" + archiveCodecExtension(result->codec);
            const auto localPath = localModel_->currentPath().value() / filename;

            operationQueue_->enqueueArchiveDownload(
                std::move(entries),
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
                            .headerText = "Archive Download Failed",
                            .text = info,
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                        return;
                    }
                    Log::info(
                        "Archive download queued as {} (destination {})",
                        opId->value(),
                        localPath.generic_string()
                    );
                }
            );
        },
    });
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
        .initialValue = item.path.filename().string(),
        .isPassword = false,
        .onConfirm = [this, item](std::optional<std::string> const& name)
        {
            if (!name || name->empty())
                return;
            if (*name == item.path.filename().string())
                return;
            if (name->find('/') != std::string::npos)
            {
                Log::error("Invalid name (cannot contain slashes): {}", *name);
                return;
            }

            Log::info("Renaming item to: {}", *name);

            std::filesystem::path fullFrom = currentPath_.value() / item.path;
            std::filesystem::path fullTo = currentPath_.value() / *name;

            fileEngine_->rename(
                fullFrom,
                fullTo,
                [this, fullFrom, fullTo](bool success, std::string const& info)
                {
                    if (!success)
                    {
                        Log::error(
                            "Failed to rename item '{}' to '{}': {}",
                            fullFrom.generic_string(),
                            fullTo.generic_string(),
                            info
                        );
                        confirmDialog_->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = "",
                            .text = fmt::format(
                                fmt::runtime(language->get("remoteSideModel", "failedToRenameItem")),
                                fullFrom.generic_string(),
                                fullTo.generic_string(),
                                info
                            ),
                            .buttons = ConfirmDialog::Button::Ok,
                        });
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
    std::string normalizedDirStr = instanceDirStr;
    std::replace(normalizedDirStr.begin(), normalizedDirStr.end(), '\\', '/');
    std::filesystem::path instanceDir{normalizedDirStr};

    Log::info("FileTracking: createInstance succeeded, id={}, dir={}", instanceId.value(), instanceDirStr);

    std::filesystem::path remotePath = currentPath_.value() / item.path;
    std::filesystem::path localPath = instanceDir / item.path.filename();

    NuiFileExplorer::Item remoteItem = item;
    remoteItem.path = remotePath;
    remoteItem.fullPath = remotePath;
    NuiFileExplorer::Item localItem = item;
    localItem.path = localPath;
    // FileEngine::addDownload prefers `fullPath` over `path` when serializing
    // the destination. Without overwriting it here, `localItem.fullPath` would
    // still hold the source item's *remote* fullPath (carried over by the
    // copy-construction above), and the backend would receive the remote path
    // as both source and destination — making it try to write
    // `<remotePath>.filepart` next to the remote file (permission denied).
    localItem.fullPath = localPath;

    operationQueue_->enqueueDownload(
        remoteItem,
        localItem,
        [this, instanceId, instanceDir, remotePath, localPath, openWith, synchronize](
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
                [this, instanceId, instanceDir, remotePath, localPath, openWith, synchronize](bool success)
                {
                    onFileDownloadComplete(
                        instanceId, instanceDir, remotePath, localPath, openWith, synchronize, success
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
    std::filesystem::path const& instanceDir,
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    bool openWith,
    bool synchronize,
    bool success
)
{
    if (!success)
    {
        Log::error("FileTracking: download failed for {}", localPath.generic_string());
        return;
    }
    Log::info("FileTracking: download completed for {}", localPath.generic_string());

    if (!synchronize)
    {
        Log::info("FileTracking: opening {} (openWith={}, no sync)", localPath.generic_string(), openWith);
        Nui::RpcClient::callWithBackChannel(
            "RpcFilesystem::open",
            [](Nui::val result)
            {
                if (!result.hasOwnProperty("success") || !result["success"].as<bool>())
                    Log::error("RpcFilesystem::open failed");
            },
            localPath.generic_string(),
            openWith
        );
        return;
    }

    Nui::RpcClient::callWithBackChannel(
        "FileTracking::addWatch",
        [this, instanceId, instanceDir, remotePath, localPath, openWith](Nui::val watchResponse)
        {
            if (!watchResponse.hasOwnProperty("success") || !watchResponse["success"].as<bool>())
            {
                Log::error("FileTracking: addWatch failed for instance {}", instanceId.value());
                return;
            }
            Log::info("FileTracking: addWatch succeeded for instance {}", instanceId.value());
            onFileWatchAdded(instanceId, instanceDir, remotePath, localPath, openWith);
        },
        instanceId.value(),
        instanceDir.generic_string(),
        true
    );
}

void RemoteSideModel::onFileWatchAdded(
    Ids::InstanceId const& instanceId,
    std::filesystem::path const& instanceDir,
    std::filesystem::path const& remotePath,
    std::filesystem::path const& localPath,
    bool openWith
)
{
    fileTracking_->startWatching(instanceId, instanceDir, remotePath, localPath);

    Log::info("FileTracking: opening {} (openWith={})", localPath.generic_string(), openWith);

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::open",
        [](Nui::val result)
        {
            if (!result.hasOwnProperty("success") || !result["success"].as<bool>())
                Log::error("RpcFilesystem::open failed");
        },
        localPath.generic_string(),
        openWith
    );
}

// --- IPlacesProvider ---

void RemoteSideModel::setRemoteUsername(std::string username)
{
    remoteUsername_ = std::move(username);
}

void RemoteSideModel::requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback)
{
    const std::string home = "/home/" + remoteUsername_;
    const std::vector<std::pair<std::string, std::string>> defaults = {
        {"Home",      home},
        {"Desktop",   home + "/Desktop"},
        {"Downloads", home + "/Downloads"},
        {"Documents", home + "/Documents"},
        {"Pictures",  home + "/Pictures"},
        {"Videos",    home + "/Videos"},
        {"Music",     home + "/Music"},
    };

    std::vector<PlaceEntry> entries;
    entries.reserve(defaults.size());
    for (auto const& [name, path] : defaults)
        entries.push_back({.icon = iconForPlaceName(name), .name = name, .path = path});

    callback(std::move(entries));
}