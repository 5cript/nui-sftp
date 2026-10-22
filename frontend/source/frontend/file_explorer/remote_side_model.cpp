#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/file_explorer/open_local_file.hpp>
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
                  lifetime_.guarded(
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
                                  listing.emplace_back(std::move(entry).name);
                          }

                          onResultsAvailable(listing);
                      }
                  )
              );
          },
          0.,
      }
{
    setFavorites(initialFavorites);
}

std::vector<NuiFileExplorer::ContextMenuItem>
RemoteSideModel::contextMenuItems(
    std::vector<NuiFileExplorer::Item> const& selectedItems,
    std::vector<NuiFileExplorer::Item> const& /*otherSideSelectedItems*/
)
{
    namespace Snc = ScriptNuiComponents;
    const bool hasItems = !selectedItems.empty();
    const bool singleItem = selectedItems.size() == 1;

    std::vector<NuiFileExplorer::ContextMenuItem> items;

    std::ranges::move(favoriteMenuItems(selectedItems), std::back_inserter(items));

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
                onDownloadAndOpen(selectedItems.front(), false, false);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWith"),
            Ui5Icons::open_command_field(),
            [this, selectedItems]()
            {
                onDownloadAndOpen(selectedItems.front(), true, false);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openAndWatch"),
            Ui5Icons::show(),
            [this, selectedItems]()
            {
                onDownloadAndOpen(selectedItems.front(), false, true);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen,
            /*shortcut=*/{},
            language->get("fileExplorer", "contextMenu", "openAndWatchHint")
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWithAndWatch"),
            Ui5Icons::show(),
            [this, selectedItems]()
            {
                onDownloadAndOpen(selectedItems.front(), true, true);
            },
            !singleItem || fileTracking_ == nullptr || !canOpen,
            /*shortcut=*/{},
            language->get("fileExplorer", "contextMenu", "openWithAndWatchHint")
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
    return item.nameSegment()
        .and_then(
            [&item](Paths::PathSegment const&)
            {
                return Paths::RemotePath::make(item.fullPath.generic_string());
            }
        );
}

std::optional<std::filesystem::path> RemoteSideModel::parentOf(std::filesystem::path const& directory) const
{
    return FileExplorer::parentOf<Paths::RemotePath>(directory.generic_string());
}

std::expected<std::string, Paths::PathError> RemoteSideModel::itemPathText(NuiFileExplorer::Item const& item) const
{
    return remotePathOf(item).transform(
        [](Paths::RemotePath const& path)
        {
            return path.value();
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
            onDownloadAndOpen(item, false, false);
        return;
    }

    Log::debug("Remote onActivateItem: {}.", item.displayName());
    navigateInto(item);
}

void RemoteSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{
    CHECK_COMPLETE();

    Log::info("New item requested: {}", static_cast<int>(type));
    promptNewItem<Paths::RemotePath>(
        type,
        [this, type](Paths::RemotePath const& path)
        {
            const bool isDirectory = type == NuiFileExplorer::Item::Type::Directory;
            auto onCreated = lifetime_.guarded(
                [this, isDirectory](bool success, std::string const& info)
                {
                    if (success)
                        return navigateTo(currentPath_.value());
                    Log::error("Failed to create {}: {}", isDirectory ? "directory" : "file", info);
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = isDirectory ? language->get("remoteSideModel", "createDirectoryFailedTitle")
                                                  : language->get("remoteSideModel", "createFileFailedTitle"),
                        .text = language->translate(info),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                }
            );
            if (isDirectory)
                return fileEngine_->createDirectory(path, std::move(onCreated));
            fileEngine_->createFile(path, std::move(onCreated));
        }
    );
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
            .onClose = lifetime_.guarded(
                [this,
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
                        confirmedToDelete.insert(
                            confirmedToDelete.end(),
                            std::make_move_iterator(std::next(nonEmpties.begin(), currentNonEmpty)),
                            std::make_move_iterator(nonEmpties.end())
                        );
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
                }
            )}
    );
}

void RemoteSideModel::enqueueDeletes(
    std::vector<Paths::RemotePath> nonEmpties,
    std::vector<Paths::RemotePath> filesAndEmptyDirs
)
{
    // The backend deletes an entry marked as directory with its content.
    std::vector<SharedData::BulkDeleteEntry> entries;
    entries.reserve(filesAndEmptyDirs.size() + nonEmpties.size());
    const auto append = [&entries](std::vector<Paths::RemotePath>& paths, bool isDirectory)
    {
        std::ranges::transform(
            paths,
            std::back_inserter(entries),
            [isDirectory](Paths::RemotePath& path)
            {
                return SharedData::BulkDeleteEntry{.target = std::move(path), .isDirectory = isDirectory};
            }
        );
    };
    append(filesAndEmptyDirs, false);
    append(nonEmpties, true);

    if (entries.empty())
        return;

    operationQueue_->enqueueBulkDelete(
        std::move(entries),
        /*insertRefresh*/ true,
        SharedData::OperationMode::Queued,
        /*onBulkComplete*/ {},
        lifetime_.guarded(
            [this](bool success, std::string const& info)
            {
                if (!success)
                {
                    Log::error("Bulk delete failed: {}", info);
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = language->get("remoteSideModel", "deleteFailed"),
                        .text = fmt::format(
                            fmt::runtime(language->get("remoteSideModel", "failedToCreateDeleteOperation")),
                            language->translate(info)
                        ),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                }
            }
        )
    );
}

void RemoteSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    Log::info("Delete items requested: {}", items.size());

    if (items.empty())
    {
        Log::error("No items selected for deletion");
        return;
    }

    const auto confirmText = items.size() > 1
        ? fmt::format(fmt::runtime(language->get("remoteSideModel", "deleteMultipleConfirm")), items.size())
        : fmt::format(fmt::runtime(language->get("remoteSideModel", "deleteSingleConfirm")), items.front().name);

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Warning,
            .headerText = language->get("remoteSideModel", "deleteRemoteItemsQuestion"),
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::No,
            .listItems = pathList(items),
            .onClose = lifetime_.guarded(
                [this, items](std::optional<ConfirmDialog::Button> button)
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
                                    fmt::runtime(language->get("remoteSideModel", "failedToDeleteItems")), item.name
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
                        lifetime_.guarded(
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
                            }
                        ),
                        lifetime_.guarded(
                            [this](
                                std::vector<Paths::RemotePath> filesAndEmptyDirs,
                                std::vector<Paths::RemotePath> nonEmpties
                            )
                            {
                                askNonEmptyDirectoryDeletions(
                                    {}, std::move(nonEmpties), std::move(filesAndEmptyDirs), 0
                                );
                            }
                        )
                    );
                }
            )}
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
        lifetime_.guarded(
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
            }
        ),
        allowOverwrite,
        insertRefresh
    );
}

void RemoteSideModel::enqueueDownloads(std::vector<SharedData::BulkDownloadEntry> accepted)
{
    // One file shows as a single transfer card with progress and resume, directories need the scan of a bulk.
    if (accepted.size() == 1 && !accepted.front().isDirectory)
        return enqueueSingleDownload(accepted.front(), accepted.front().mayOverwrite, true);
    operationQueue_->enqueueBulkDownload(
        std::move(accepted),
        /*allowOverwrite*/ false,
        /*insertRefresh*/ true,
        SharedData::OperationMode::Queued,
        /*onEachComplete*/ {},
        lifetime_.guarded(
            [this](bool success, std::string const& info)
            {
                if (success)
                    return;
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
        )
    );
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
        [&subDir](Paths::LocalPath const& directory)
        {
            return FileExplorer::targetDirectoryOf(directory, subDir);
        }
    );
    if (!destinationDirectory)
        return refuse(destinationDirectory.error());
    // A name this platform cannot hold is refused before anything is written.
    auto candidates = transferCandidates<SharedData::BulkDownloadEntry>(
        items,
        [this](NuiFileExplorer::Item const& item)
        {
            return remotePathOf(item);
        },
        *destinationDirectory
    );
    if (!candidates)
        return refuse(candidates.error());

    confirmTransfer<SharedData::BulkDownloadEntry>(
        *std::move(candidates),
        {
            .headerText = language->get("remoteSideModel", "downloadItemsQuestion"),
            .singleText = language->get("remoteSideModel", "downloadConfirmIfSingle"),
            .multipleText = language->get("remoteSideModel", "downloadConfirmIfMultiple"),
            .destinationDirectory = destinationDirectory->toUtf8(),
            .overwriteHeaderText = language->get("remoteSideModel", "fileAlreadyExistsOverwrite"),
            .overwriteText = language->get("remoteSideModel", "allowOverwritingFile"),
        },
        lifetime_.guarded(
            [this](std::vector<SharedData::BulkDownloadEntry> accepted)
            {
                enqueueDownloads(std::move(accepted));
            }
        )
    );
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
        items.size() == 1 ? items.front().name : language->get("fileExplorer", "archiveDefaultName");

    archiveTransferDialog_->open({
        .headerText = language->get("fileExplorer", "contextMenu", "downloadAsArchive"),
        .initialFileStem = defaultStem,
        .initialCodec = ArchiveCodec::Gzip,
        .initialCompressionLevel = 5,
        .onConfirm = lifetime_.guarded(
            [this, roots = std::move(roots)](std::optional<ArchiveTransferResult> const& result) mutable
            {
                if (!result)
                {
                    Log::info("Download as Archive: user cancelled");
                    return;
                }
                if (!localModel_)
                    return;

                const auto filename = fmt::format("{}.tar{}", result->fileStem, archiveCodecExtension(result->codec));
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
                    return showPathRefused(
                        language->get("remoteSideModel", "archiveDownloadFailed"), archivePath.error()
                    );
                const auto localPath = *std::move(archivePath);

                operationQueue_->enqueueArchiveDownload(
                    std::move(roots),
                    localPath,
                    backendCompressionCode(result->codec),
                    result->compressionLevel,
                    /*mayOverwrite=*/false,
                    SharedData::OperationMode::Queued,
                    lifetime_.guarded(
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
                            Log::info(
                                "Archive download queued as {} (destination {})", opId->value(), localPath.toUtf8()
                            );
                        }
                    )
                );
            }
        ),
    });
}

void RemoteSideModel::onRename(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Rename item requested: {}", item.name);
    // The item's own path: the explorer may already show another directory.
    promptRename(item, remotePathOf(item));
}

void RemoteSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    CHECK_COMPLETE();

    Log::info("Properties requested: {}", item.name);

    const auto path = remotePathOf(item);
    filePropertyDialog_->open(
        static_cast<SharedData::DirectoryEntry const&>(item),
        path ? path->value() : std::string{item.displayName()},
        item.linkTarget.value_or(std::string{})
    );
}

void RemoteSideModel::navigateTo(std::filesystem::path const& path)
{
    CHECK_COMPLETE();

    const auto target = FileExplorer::navigationTarget<Paths::RemotePath>(path.generic_string());
    Log::info("Navigating to: {}", target);
    preNavigatePath_ = currentPath_.value();
    currentPath_ = std::filesystem::path{target};
    fileEngine_->listDirectory(
        currentPath_.value(),
        lifetime_.guarded(
            [this, directory = currentPath_.value()](
                std::optional<std::vector<SharedData::DirectoryEntry>> entries, std::string const& info
            )
            {
                onDirectoryListing(directory, std::move(entries), info);
            }
        )
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
            std::ranges::transform(
                suggestions,
                suggestions.begin(),
                [directory = path.parent_path()](std::filesystem::path const& suggestion)
                {
                    return directory / suggestion;
                }
            );
            onResultsAvailable(suggestions);
        }
    );
}

void RemoteSideModel::onDownloadAndOpen(NuiFileExplorer::Item const& item, bool openWith, bool watch)
{
    CHECK_COMPLETE();

    if (fileTracking_ == nullptr)
    {
        Log::error("File tracking panel is not available, cannot download/open file.");
        return;
    }
    const auto failedTitle = language->get("remoteSideModel", watch ? "openAndWatchFailedTitle" : "downloadFailed");
    const auto remotePath = remotePathOf(item);
    if (!remotePath)
        return showPathRefused(failedTitle, remotePath.error());
    fileTracking_->open(
        *remotePath,
        item.size,
        watch,
        lifetime_.guarded(
            [this, openWith](Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
            {
                Log::info("FileTracking: opening {} (openWith={})", localPath.toUtf8(), openWith);
                FileExplorer::openLocalFile(
                    lifetime_, *confirmDialog_, localPath, openWith, discardWhenDeclined(instanceId, localPath)
                );
            }
        ),
        lifetime_.guarded(
            [this, failedTitle](std::string const& reason)
            {
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = failedTitle,
                    .text = reason,
                    .buttons = ConfirmDialog::Button::Ok,
                });
            }
        )
    );
}

std::function<void()>
RemoteSideModel::discardWhenDeclined(Ids::InstanceId const& instanceId, Paths::LocalPath const& localPath)
{
    return [this, instanceId, localPath]()
    {
        if (fileTracking_ != nullptr)
            fileTracking_->discard(instanceId, localPath);
    };
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
        lifetime_.guarded(
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
        )
    );
}

void RemoteSideModel::placesFromHomeListing()
{
    fileEngine_->listDirectory(
        remoteHome_,
        lifetime_.guarded(
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
                            return isDirectory && entry.name == place.directoryName;
                        }
                    );
                    if (exists)
                        remotePlaces_->emplace_back(std::string{place.kind}, remoteHome_ / place.directoryName);
                }
                finishPlacesRequests();
            }
        )
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

    std::ranges::transform(
        *remotePlaces_,
        std::back_inserter(entries),
        [](std::pair<std::string, std::filesystem::path> const& place)
        {
            return PlaceEntry{
                .icon = iconForPlaceKind(place.first),
                .name = placeDisplayName(place.first, place.first),
                .path = place.second.generic_string(),
            };
        }
    );
    return entries;
}