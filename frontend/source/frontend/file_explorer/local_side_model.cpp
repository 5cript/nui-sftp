#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/remote_side_model.hpp>
#include <frontend/file_explorer/file_explorer_texts.hpp>
#include <frontend/file_explorer/place_names.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <script-nui-components/popup_menu.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <ui5-sap-icons/icons/database.hpp>
#include <ui5-sap-icons/icons/upload.hpp>
#include <ui5-sap-icons/icons/open-folder.hpp>
#include <ui5-sap-icons/icons/open-command-field.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/detail-view.hpp>
#include <ui5-sap-icons/icons/show.hpp>
#include <ui5-sap-icons/icons/add-favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/val_conversion.hpp>
#include <nui/rpc.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::string_literals;

LocalSideModel::LocalSideModel(
    Persistence::UiOptions uiOptions,
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

              Nui::val args = Nui::val::object();
              args.set("path", dirPath.generic_string());
              args.set("fileNameOnly", false);

              Nui::RpcClient::callWithBackChannel(
                  "RpcFilesystem::listFiles",
                  [onResultsAvailable = std::move(onResultsAvailable)](Nui::val val)
                  {
                      if (!val.hasOwnProperty("success"))
                      {
                          Log::error("Invalid response from RpcFilesystem::listFiles");
                          Nui::WebApi::Console::error(val);
                          onResultsAvailable({});
                          return;
                      }

                      const auto success = val["success"].as<bool>();
                      if (!success)
                      {
                          const auto error = language->translate(val["error"].as<std::string>());
                          Log::error("Failed to list files: {}", error);
                          onResultsAvailable({});
                          return;
                      }

                      std::vector<std::filesystem::path> listing{};
                      const auto files = val["files"];
                      for (const auto& file : files)
                      {
                          // Dirs only
                          if (static_cast<std::filesystem::file_type>(file["type"].as<int>()) ==
                              std::filesystem::file_type::directory)
                          {
                              listing.emplace_back(file["path"].as<std::string>());
                          }
                      }

                      onResultsAvailable(listing);
                  },
                  args
              );
          },
          0.,
      }
    , favorites_{std::make_shared<Nui::Observed<std::vector<std::filesystem::path>>>(
          [this]()
          {
              std::vector<std::filesystem::path> paths;
              paths.reserve(uiOptions_.localFavorites.size());
              for (auto const& str : uiOptions_.localFavorites)
                  paths.emplace_back(str);
              return paths;
          }()
      )}
{
    probeOpenerCapabilities();
}

void LocalSideModel::probeOpenerCapabilities()
{
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::openerCapabilities",
        [this](Nui::val val)
        {
            if (!val.hasOwnProperty("success") || !val["success"].as<bool>())
            {
                Log::warn("RpcFilesystem::openerCapabilities failed or unavailable; assuming capable.");
                return;
            }

            SharedData::OpenerCapabilities caps{};
            try
            {
                Nui::convertFromVal(val["capabilities"], caps);
            }
            catch (std::exception const& e)
            {
                Log::warn("Failed to parse openerCapabilities response: {}", e.what());
                return;
            }

            openerCaps_ = caps;
            Log::info(
                "Opener capabilities: canOpenFile={}, canOpenInFileManager={}, reason='{}'",
                caps.canOpenFile,
                caps.canOpenInFileManager,
                caps.reason
            );

            if (!caps.canOpenFile || !caps.canOpenInFileManager)
            {
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
                    .headerText = language->get("localSideModel", "portalUnavailableHeader"),
                    .text = fmt::format(
                        fmt::runtime(language->get("localSideModel", "portalUnavailableText")),
                        language->translate(caps.reason)
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                    .neverShowAgainId = "localSideModel.portalUnavailable",
                });
            }
        }
    );
}

void LocalSideModel::setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback)
{
    onFavoritesChanged_ = std::move(callback);
}

// --- IPlacesProvider ---

namespace
{
    struct LocalPlace
    {
        std::string kind;
        std::string name;
        std::string path;
    };

    /**
     * @brief The local default places, asked from the backend once and shared by every session.
     */
    std::optional<std::vector<LocalPlace>> localPlacesCache{};

    /**
     * @brief Requests waiting for the one backend call in flight.
     */
    std::vector<std::function<void(std::vector<LocalPlace> const&)>> pendingLocalPlacesRequests{};

    std::vector<LocalSideModel::PlaceEntry> toPlaceEntries(std::vector<LocalPlace> const& places)
    {
        std::vector<LocalSideModel::PlaceEntry> entries;
        entries.reserve(places.size());
        for (auto const& place : places)
        {
            entries.push_back({
                .icon = iconForPlaceKind(place.kind),
                .name = placeDisplayName(place.kind, place.name),
                .path = place.path,
            });
        }
        return entries;
    }

    void finishLocalPlacesRequests(std::vector<LocalPlace> const& places)
    {
        for (auto const& pending : std::exchange(pendingLocalPlacesRequests, {}))
            pending(places);
    }
}

void LocalSideModel::requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback)
{
    if (localPlacesCache)
        return callback(toPlaceEntries(*localPlacesCache));

    pendingLocalPlacesRequests.push_back(
        [callback = std::move(callback)](std::vector<LocalPlace> const& places)
        {
            callback(toPlaceEntries(places));
        }
    );
    if (pendingLocalPlacesRequests.size() > 1)
        return;

    Nui::RpcClient::callWithBackChannel(
        "NuiFileExplorer::DefaultPlaces::list",
        [](Nui::val val)
        {
            if (!val.hasOwnProperty("success") || !val["success"].as<bool>())
            {
                Log::error("DefaultPlaces::list failed");
                finishLocalPlacesRequests({});
                return;
            }

            std::vector<LocalPlace> places;
            const auto list = val["places"];
            const auto length = list["length"].as<int>();
            places.reserve(length);
            for (int index = 0; index < length; ++index)
            {
                places.push_back({
                    .kind = list[index]["kind"].as<std::string>(),
                    .name = list[index]["name"].as<std::string>(),
                    .path = list[index]["path"].as<std::string>(),
                });
            }
            localPlacesCache = places;
            finishLocalPlacesRequests(places);
        }
    );
}

bool LocalSideModel::showRootEntry() const
{
    return STRINGIZE_EXPANDED(BROWSER_ENGINE) != "webview2"s;
}

// --- IDrivesProvider ---

NuiFileExplorer::IDrivesProvider* LocalSideModel::drivesProvider()
{
    if (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webview2"s)
        return this;
    else
        return nullptr;
}

void LocalSideModel::requestDrives(std::function<void(std::vector<PlaceEntry>)> callback)
{
    if (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webview2"s)
    {
        Nui::RpcClient::callWithBackChannel(
            "NuiFileExplorer::Drives::list",
            [callback = std::move(callback)](Nui::val val)
            {
                if (!val.hasOwnProperty("success") || !val["success"].as<bool>())
                {
                    Log::error("Drives::list failed");
                    callback({});
                    return;
                }
                std::vector<PlaceEntry> entries;
                const auto drives = val["drives"];
                const auto len = drives["length"].as<int>();
                entries.reserve(len);
                for (int idx = 0; idx < len; ++idx)
                {
                    entries.push_back({
                        .icon = Ui5Icons::database(),
                        .name = drives[idx]["name"].as<std::string>(),
                        .path = drives[idx]["path"].as<std::string>(),
                    });
                }
                callback(std::move(entries));
            }
        );
        return;
    }

    callback({});
}

// --- IFavoritesProvider ---

std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> LocalSideModel::favorites() const
{
    return favorites_;
}

void LocalSideModel::addFavorite(std::filesystem::path const& path)
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

void LocalSideModel::removeFavorite(std::filesystem::path const& path)
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

void LocalSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    if (!item.isDirectoryLike())
    {
        if (openerCaps_.canOpenFile)
            onOpen(item, false);
        return;
    }

    // The item's own path, the side may already be loading another directory.
    Log::debug("Local onActivateItem: {}.", item.displayName());
    navigateTo(item.fullPath.empty() ? currentPath_.value() / item.name : item.fullPath);
}
void LocalSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{
    inputDialog_->open(
        {.whatFor = language->get(
             "remoteSideModel", type == NuiFileExplorer::Item::Type::Directory ? "newDirectory" : "newFile"
         ),
            .prompt = language->get(
                "remoteSideModel",
                type == NuiFileExplorer::Item::Type::Directory ? "enterNewDirectoryName" : "enterNewFileName"
            ),
            .headerText = language->get(
                "remoteSideModel", type == NuiFileExplorer::Item::Type::Directory ? "createNewDirectory" : "createNewFile"
            ),
            .isPassword = false,
            .onConfirm = [this, type](std::optional<std::string> const& name)
            {
                if (!name)
                    return;

                if (!isValidItemName(*name))
                    return showInvalidItemName(*name);
                auto path = currentLocalPath().and_then(
                    [&name](Paths::LocalPath const& directory)
                    {
                        return Paths::LocalPathSegment::make(*name).transform(
                            [&directory](Paths::LocalPathSegment const& segment)
                            {
                                return directory / segment;
                            }
                        );
                    }
                );
                if (!path)
                    return showPathRefused(language->get("localSideModel", "createItemFailedTitle"), path.error());
                Log::info("Creating new item: {}", path->toUtf8());

                if (type == NuiFileExplorer::Item::Type::Directory)
                {
                    Nui::val args = Nui::val::object();
                    args.set("path", path->toUtf8());

                    Nui::RpcClient::callWithBackChannel(
                        "RpcFilesystem::createDirectory",
                        [this](Nui::val val)
                        {
                            if (!val.hasOwnProperty("success"))
                            {
                                Log::error("Invalid response from RpcFilesystem::createDirectory");
                                confirmDialog_->open({
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("localSideModel", "createDirectoryFailedTitle"),
                                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }

                            const auto success = val["success"].as<bool>();
                            if (!success)
                            {
                                const auto error = language->translate(val["error"].as<std::string>());
                                Log::error("Failed to create directory: {}", error);
                                confirmDialog_->open({
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("localSideModel", "createDirectoryFailedTitle"),
                                    .text = error,
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }

                            Log::info("Directory created successfully");
                            // Refresh list:
                            navigateTo(currentPath_.value());
                        },
                        args
                    );
                }
                else if (type == NuiFileExplorer::Item::Type::Regular)
                {
                    Nui::val args = Nui::val::object();
                    args.set("filePath", path->toUtf8());

                    Nui::RpcClient::callWithBackChannel(
                        "RpcFilesystem::createFile",
                        [this](Nui::val val)
                        {
                            if (!val.hasOwnProperty("success"))
                            {
                                Log::error("Invalid response from RpcFilesystem::createFile");
                                confirmDialog_->open({
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("localSideModel", "createFileFailedTitle"),
                                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }

                            const auto success = val["success"].as<bool>();
                            if (!success)
                            {
                                const auto error = language->translate(val["error"].as<std::string>());
                                Log::error("Failed to create file: {}", error);
                                confirmDialog_->open({
                                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                    .headerText = language->get("localSideModel", "createFileFailedTitle"),
                                    .text = error,
                                    .buttons = ConfirmDialog::Button::Ok,
                                });
                                return;
                            }

                            Log::info("File created successfully");
                            // Refresh list:
                            navigateTo(currentPath_.value());
                        },
                        args
                    );
                }
                else
                {
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = language->get("localSideModel", "createItemFailedTitle"),
                        .text = language->get("localSideModel", "unsupportedItemType"),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                }
            }}
    );
}
void LocalSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    if (items.empty())
        return;

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("remoteSideModel", "deleteItemsQuestion"),
            .text = language->get("localSideModel", "deleteSelectedConfirm"),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items](std::optional<ConfirmDialog::Button> button)
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Delete items cancelled");
                    return;
                }

                Nui::val args = Nui::val::object();
                args.set("paths", Nui::val::array());
                args.set("recursive", true);

                for (const auto& item : items)
                {
                    auto path = localPathOf(item);
                    if (!path)
                        return showPathRefused(language->get("localSideModel", "deleteFilesFailedTitle"), path.error());
                    args["paths"].call<void>("push", path->toUtf8());
                }

                Nui::RpcClient::callWithBackChannel(
                    "RpcFilesystem::removeSome",
                    [this](Nui::val val)
                    {
                        if (!val.hasOwnProperty("success"))
                        {
                            Log::error("Invalid response from RpcFilesystem::remove");
                            confirmDialog_->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = language->get("localSideModel", "deleteFilesFailedTitle"),
                                .text = language->get("localSideModel", "invalidResponseFromBackend"),
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                            return;
                        }

                        const auto success = val["success"].as<bool>();
                        if (!success)
                        {
                            const auto error = language->translate(val["error"].as<std::string>());
                            Log::error("Failed to delete files: {}", error);
                            confirmDialog_->open({
                                .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                                .headerText = language->get("localSideModel", "deleteFilesFailedTitle"),
                                .text = error,
                                .buttons = ConfirmDialog::Button::Ok,
                            });
                            return;
                        }

                        Log::info("Files deleted successfully");
                        // Refresh list:
                        navigateTo(currentPath_.value());
                    },
                    args
                );
            }}
    );
}
void LocalSideModel::onTransfer(
    std::vector<NuiFileExplorer::Item> const& items,
    std::optional<std::string> const& subDir
)
{
    CHECK_COMPLETE();

    if (items.empty())
        return;

    if (!remoteModel_)
    {
        Log::error("Cannot transfer items: remote model is not set");
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("localSideModel", "transferFailedTitle"),
            .text = language->get("localSideModel", "remoteSideUnavailable"),
            .buttons = ConfirmDialog::Button::Ok,
        });
        return;
    }

    Log::info("Upload items requested: {}", items.size());

    const auto refuse = [this](Paths::PathError const& error)
    {
        showPathRefused(language->get("localSideModel", "transferFailedTitle"), error);
    };
    const auto destinationDirectory = remoteModel_->currentRemotePath().and_then(
        [&subDir](Paths::RemotePath const& directory) -> std::expected<Paths::RemotePath, Paths::PathError>
        {
            if (!subDir)
                return directory;
            return Paths::PathSegment::make(*subDir).transform(
                [&directory](Paths::PathSegment const& segment)
                {
                    return directory / segment;
                }
            );
        }
    );
    if (!destinationDirectory)
        return refuse(destinationDirectory.error());

    std::vector<SharedData::BulkUploadEntry> candidates;
    candidates.reserve(items.size());
    for (auto const& item : items)
    {
        auto source = localPathOf(item);
        if (!source)
            return refuse(source.error());
        const auto name = source->filename();
        if (!name)
            return refuse(Paths::PathError{.reason = Paths::PathRejection::Empty, .input = source->toUtf8()});
        candidates.push_back(
            SharedData::BulkUploadEntry{
                .source = *source,
                .destination = *destinationDirectory / *name,
                .sizeBytes = item.size,
                .isDirectory = item.isDirectory(),
            }
        );
    }

    const auto itemsSize = candidates.size();
    std::string confirmText = fmt::format(
        fmt::runtime(
            language->get("localSideModel", itemsSize > 1 ? "uploadMultipleConfirmText" : "uploadSingleConfirmText")
        ),
        itemsSize > 1 ? std::to_string(itemsSize) : std::string{candidates.front().destination.filename()->view()},
        destinationDirectory->value()
    );

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& candidate : candidates)
        listItems.push_back({candidate.source.toUtf8(), ""});

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("localSideModel", "uploadConfirmTitle"),
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, candidates = std::move(candidates)](std::optional<ConfirmDialog::Button> button) mutable
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Upload items cancelled");
                    return;
                }

                Log::info("Uploading items");

                // Batched remote-existence probe, one RPC for the whole batch instead of N per-file SFTP stats.
                std::vector<Paths::RemotePath> destinations;
                destinations.reserve(candidates.size());
                for (auto const& candidate : candidates)
                    destinations.push_back(candidate.destination);

                fileEngine_->existsBatchRemote(
                    destinations,
                    [this,
                        candidates = std::move(candidates)](std::vector<bool> exists, std::string const& info) mutable
                    {
                        // An unknown destination is never taken for absent, the batch overwrites what it accepts.
                        if (exists.size() != candidates.size())
                            return showExistenceCheckFailed(
                                exists.empty() ? info : fmt::format("{} of {} answers", exists.size(), candidates.size())
                            );
                        uploadItemsConfirmed(
                            std::move(candidates), std::make_shared<std::vector<bool>>(std::move(exists))
                        );
                    }
                );
            }}
    );
}

void LocalSideModel::onDropExternal(
    std::vector<NuiFileExplorer::Item> const& items,
    std::optional<std::string> const& subDir,
    bool issueWebkitWarning
)
{
    if (issueWebkitWarning && STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s)
    {
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("sessionFrontend", "externalDropWebkitWarningTitle"),
            .text = language->get("sessionFrontend", "externalDropWebkitWarningText"),
            .buttons = ConfirmDialog::Button::Ok,
            .onClose = [this, items, subDir](std::optional<ConfirmDialog::Button>)
            {
                onDropExternal(items, subDir, false);
            },
        });
    }
    else
        onTransfer(items, subDir);
}

void LocalSideModel::renameItem(Paths::LocalPath const& from, Paths::LocalPath const& to, bool overwrite)
{
    Nui::val args = Nui::val::object();
    args.set("oldPath", from.toUtf8());
    args.set("newPath", to.toUtf8());
    args.set("overwrite", overwrite);

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::rename",
        [this, from, to, overwrite](Nui::val val)
        {
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::rename");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "renameFailed"),
                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            if (!val["success"].as<bool>())
            {
                const auto errorKey = val["error"].as<std::string>();
                if (errorKey == "backend.rename.targetExists" && !overwrite)
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
                const auto error = language->translate(errorKey);
                Log::error("Failed to rename item: {}", error);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "renameFailed"),
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            Log::info("Item renamed successfully");
            onRefresh();
        },
        args
    );
}

void LocalSideModel::onRename(NuiFileExplorer::Item const& item)
{
    Log::info("Rename item requested: {}", item.name);

    // The item's own path: the explorer may already show another directory.
    auto from = localPathOf(item);
    if (!from)
        return showPathRefused(language->get("localSideModel", "renameFailed"), from.error());

    inputDialog_->open({
        .whatFor = language->get("remoteSideModel", "rename"),
        .prompt = fmt::format(fmt::runtime(language->get("remoteSideModel", "renamePrompt")), item.path.filename().string()),
        .headerText =
            fmt::format(fmt::runtime(language->get("remoteSideModel", "renameWithItem")), item.path.filename().string()),
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
            const auto segment = Paths::LocalPathSegment::make(*name);
            const auto directory = from.parent();
            if (!segment || !directory)
                return showInvalidItemName(*name);

            Log::info("Renaming item to: {}", *name);
            renameItem(from, *directory / *segment, false);
        },
    });
}
void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    // listFiles only carries the lean field set; fetch the full entry (atime, birthtime, acl, ...)
    // on demand so the dialog can display everything without inflating every directory listing.
    auto fallback = static_cast<SharedData::DirectoryEntry>(item);
    auto path = localPathOf(item);
    if (!path)
    {
        Log::warn("Properties of '{}' use the listed entry: {}", item.name, path.error().toString());
        filePropertyDialog_->open(fallback, item.displayName());
        return;
    }
    Nui::val args = Nui::val::object();
    args.set("path", path->toUtf8());

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::properties",
        [this, fallback = std::move(fallback)](Nui::val val) mutable
        {
            const auto useFallback = [this, &fallback]()
            {
                filePropertyDialog_->open(fallback);
            };

            if (!val.hasOwnProperty("success") || !val["success"].as<bool>())
            {
                Log::warn("RpcFilesystem::properties failed; opening dialog with cached entry");
                useFallback();
                return;
            }
            if (!val.hasOwnProperty("entry") || val["entry"].isNull() || val["entry"].isUndefined())
            {
                useFallback();
                return;
            }

            const auto extractFull = [](Nui::val const& source) -> SharedData::DirectoryEntry
            {
                const auto pickU64 = [&source](char const* key) -> std::uint64_t
                {
                    if (!source.hasOwnProperty(key))
                        return 0;
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return 0;
                    return field.template as<std::uint64_t>();
                };
                const auto pickU32 = [&source](char const* key) -> std::uint32_t
                {
                    if (!source.hasOwnProperty(key))
                        return 0;
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return 0;
                    return field.template as<std::uint32_t>();
                };
                const auto pickString = [&source](char const* key) -> std::string
                {
                    if (!source.hasOwnProperty(key))
                        return {};
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return {};
                    return field.template as<std::string>();
                };

                SharedData::DirectoryEntry out{
                    .path = source["path"].template as<std::string>(),
                    .type = SharedData::fileTypeFromStdFilesystemType(
                        static_cast<std::filesystem::file_type>(source["type"].template as<int>())
                    ),
                    .size = pickU64("size"),
                    .uid = pickU32("uid"),
                    .gid = pickU32("gid"),
                    .owner = pickString("owner"),
                    .group = pickString("group"),
                    .permissions = static_cast<std::filesystem::perms>(pickU32("permissions")),
                    .atime = pickU64("atime"),
                    .atimeNsec = pickU32("atimeNsec"),
                    .createTime = pickU64("createTime"),
                    .createTimeNsec = pickU32("createTimeNsec"),
                    .mtime = pickU64("mtime"),
                    .mtimeNsec = pickU32("mtimeNsec"),
                    .acl = pickString("acl"),
                };
                if (source.hasOwnProperty("linkTarget"))
                {
                    const auto link = source["linkTarget"];
                    if (!link.isNull() && !link.isUndefined())
                        out.linkTarget = std::filesystem::path{link.template as<std::string>()};
                }
                return out;
            };

            const auto entryVal = val["entry"];
            auto full = extractFull(entryVal);
            if (full.type == SharedData::FileType::Symlink && entryVal.hasOwnProperty("resolvedTarget"))
            {
                const auto target = entryVal["resolvedTarget"];
                if (!target.isNull() && !target.isUndefined())
                    full.resolvedTarget = std::make_shared<SharedData::DirectoryEntry>(extractFull(target));
            }

            filePropertyDialog_->open(full);
        },
        args
    );
}
void LocalSideModel::onError(NuiFileExplorer::Error const& fileExplorerError)
{
    const auto error = fileExplorerErrorText(fileExplorerError);
    Log::error("File grid error (local side): {}", error);
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("localSideModel", "fileGridError"),
        .text = error,
        .buttons = ConfirmDialog::Button::Ok,
    });
}

bool LocalSideModel::hasParent(std::filesystem::path const& directory) const
{
    // A drive root like "C:/" has no parent either.
    return Paths::LocalPath::fromUtf8(directory.generic_string())
        .transform(
            [](Paths::LocalPath const& path)
            {
                return !path.isRoot();
            }
        )
        .value_or(false);
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
        [this, directory = currentPath_.value()](Nui::val val)
        {
            Log::debug("Received directory listing response");
            if (directory != currentPath_.value())
            {
                Log::debug("Dropping the listing of '{}', the side moved on.", directory.generic_string());
                return;
            }
            auto revertNavigation = [this]()
            {
                if (currentPath_.value() != preNavigatePath_)
                {
                    currentPath_ = preNavigatePath_;
                    currentPath_.modifyNow();
                }
                // Re-render the existing items so the side's loading hint clears even though
                // no new directory data arrived. updateItems preserves selection across the
                // refresh, so the user lands back on what they had before the failed nav.
                if (refreshCallback_)
                    refreshCallback_(false, true);
            };

            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::listFiles");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToListFiles"),
                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                revertNavigation();
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = language->translate(val["error"].as<std::string>());
                Log::error("Failed to list files: {}", error);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToListFiles"),
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                revertNavigation();
                return;
            }

            const auto extractEntry = [](Nui::val const& source) -> SharedData::DirectoryEntry
            {
                const auto pickU64 = [&source](char const* key) -> std::uint64_t
                {
                    if (!source.hasOwnProperty(key))
                        return 0;
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return 0;
                    return field.template as<std::uint64_t>();
                };
                const auto pickU32 = [&source](char const* key) -> std::uint32_t
                {
                    if (!source.hasOwnProperty(key))
                        return 0;
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return 0;
                    return field.template as<std::uint32_t>();
                };
                const auto pickString = [&source](char const* key) -> std::string
                {
                    if (!source.hasOwnProperty(key))
                        return {};
                    const auto field = source[key];
                    if (field.isNull() || field.isUndefined())
                        return {};
                    return field.template as<std::string>();
                };

                SharedData::DirectoryEntry entry{
                    .path = source["path"].template as<std::string>(),
                    .type = SharedData::fileTypeFromStdFilesystemType(
                        static_cast<std::filesystem::file_type>(source["type"].template as<int>())
                    ),
                    .size = pickU64("size"),
                    .uid = pickU32("uid"),
                    .gid = pickU32("gid"),
                    .owner = pickString("owner"),
                    .group = pickString("group"),
                    .permissions = static_cast<std::filesystem::perms>(pickU32("permissions")),
                    .atime = pickU64("atime"),
                    .atimeNsec = pickU32("atimeNsec"),
                    .createTime = pickU64("createTime"),
                    .createTimeNsec = pickU32("createTimeNsec"),
                    .mtime = pickU64("mtime"),
                    .mtimeNsec = pickU32("mtimeNsec"),
                };
                if (source.hasOwnProperty("linkTarget"))
                {
                    const auto link = source["linkTarget"];
                    if (!link.isNull() && !link.isUndefined())
                        entry.linkTarget = std::filesystem::path{link.template as<std::string>()};
                }
                return entry;
            };

            std::vector<SharedData::DirectoryEntry> directoryEntries;
            const auto files = val["files"];
            const auto fileCount = files["length"].as<int>();
            directoryEntries.reserve(static_cast<std::size_t>(fileCount));
            for (int idx = 0; idx < fileCount; ++idx)
            {
                const auto file = files[idx];
                auto entry = extractEntry(file);
                if (entry.type == SharedData::FileType::Symlink && file.hasOwnProperty("resolvedTarget"))
                {
                    const auto target = file["resolvedTarget"];
                    if (!target.isNull() && !target.isUndefined())
                    {
                        entry.resolvedTarget = std::make_shared<SharedData::DirectoryEntry>(extractEntry(target));
                    }
                }
                directoryEntries.push_back(std::move(entry));
            }

            onDirectoryListing(directory, std::move(directoryEntries));
        },
        args
    );
}

void LocalSideModel::uploadItemsConfirmed(
    std::vector<SharedData::BulkUploadEntry> candidates,
    std::shared_ptr<std::vector<bool>> existsResults,
    std::size_t index,
    bool overwriteNever,
    bool overwriteAlways,
    std::shared_ptr<std::vector<SharedData::BulkUploadEntry>> accepted
)
{
    // Mirrors RemoteSideModel::downloadItemsConfirmed — see that for the
    // shape rationale (iterate sync, recurse only on conflict prompts to
    // keep WASM stack depth bounded by conflicts not by file count).

    if (!accepted)
        accepted = std::make_shared<std::vector<SharedData::BulkUploadEntry>>();

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
            enqueueSingleUpload(
                accepted->front(),
                /*allowOverwrite=*/true,
                /*insertRefresh=*/true
            );
            accepted->clear();
            return;
        }
        operationQueue_->enqueueBulkUpload(
            std::move(*accepted),
            /*allowOverwrite*/ true,
            /*insertRefresh*/ true,
            SharedData::OperationMode::Queued,
            /*onEachComplete*/ {},
            [this](bool success, std::string const& info)
            {
                if (!success)
                {
                    Log::error("Bulk upload failed: {}", info);
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = language->get("localSideModel", "uploadFailed"),
                        .text = info,
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

    while (index < candidates.size())
    {
        auto const& candidate = candidates[index];
        const bool exists = (existsResults && index < existsResults->size()) ? (*existsResults)[index] : false;

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
        break;
    }

    if (index >= candidates.size())
    {
        flushAccepted();
        return;
    }

    auto const& candidate = candidates[index];
    Log::info("Uploading '{}' to '{}'.", candidate.source.toUtf8(), candidate.destination.value());

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("localSideModel", "fileAlreadyExistsOverwriteHeader"),
            .text = language->get("localSideModel", "fileAlreadyExistsOverwrite"),
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                ConfirmDialog::Button::None | ConfirmDialog::Button::Cancel,
            .focusButton = ConfirmDialog::Button::No,
            .listItems = {{.text = candidate.destination.value(), .description = language->get("fileExplorer", "fileAlreadyExists")}},
            .onClose = [this,
                           candidates = std::move(candidates),
                           existsResults,
                           index,
                           overwriteNever,
                           overwriteAlways,
                           accepted](std::optional<ConfirmDialog::Button> button) mutable
            {
                if (button && *button == ConfirmDialog::Button::Yes)
                {
                    accepted->push_back(approvedForOverwrite(candidates[index]));
                    uploadItemsConfirmed(
                        std::move(candidates),
                        std::move(existsResults),
                        index + 1,
                        overwriteNever,
                        overwriteAlways,
                        std::move(accepted)
                    );
                }
                else if (button && *button == ConfirmDialog::Button::No)
                {
                    Log::info("Skipping upload of existing file: {}", candidates[index].destination.value());
                    uploadItemsConfirmed(
                        std::move(candidates),
                        std::move(existsResults),
                        index + 1,
                        overwriteNever,
                        overwriteAlways,
                        std::move(accepted)
                    );
                }
                else if (button && *button == ConfirmDialog::Button::All)
                {
                    Log::info("Overwriting all existing files from now on.");
                    accepted->push_back(approvedForOverwrite(candidates[index]));
                    uploadItemsConfirmed(
                        std::move(candidates),
                        std::move(existsResults),
                        index + 1,
                        overwriteNever,
                        /*overwriteAlways*/ true,
                        std::move(accepted)
                    );
                }
                else if (button && *button == ConfirmDialog::Button::None)
                {
                    Log::info("Skipping all existing files from now on.");
                    uploadItemsConfirmed(
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
                    const auto terminalIndex = candidates.size();
                    uploadItemsConfirmed(
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

void LocalSideModel::enqueueSingleUpload(
    SharedData::BulkUploadEntry const& entry,
    bool allowOverwrite,
    bool insertRefresh
)
{
    operationQueue_->enqueueUpload(
        entry.source,
        entry.destination,
        entry.sizeBytes,
        [this, source = entry.source](std::optional<Ids::OperationId> const& opId, std::string const& info)
        {
            if (!opId)
            {
                Log::error("Failed to create upload operation: {}", info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "",
                    .text = fmt::format(
                        fmt::runtime(language->get("localSideModel", "failedToEnqueueUpload")), source.toUtf8(), info
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }
            Log::info("Upload operation created with id: {}", opId->value());
        },
        allowOverwrite,
        insertRefresh
    );
}

std::vector<NuiFileExplorer::ContextMenuItem>
LocalSideModel::contextMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems)
{
    namespace Snc = ScriptNuiComponents;
    const bool hasItems = !selectedItems.empty();
    const bool singleItem = selectedItems.size() == 1;

    std::vector<NuiFileExplorer::ContextMenuItem> items;

    if (remoteModel_)
    {
        items.push_back(
            Snc::PopupMenu::item(
                language->get("fileExplorer", "contextMenu", "upload"),
                Ui5Icons::upload(),
                [this, selectedItems]()
                {
                    onTransfer(selectedItems, std::nullopt);
                },
                !hasItems
            )
        );
        items.push_back(
            Snc::PopupMenu::item(
                language->get("fileExplorer", "contextMenu", "uploadAsArchive"),
                Ui5Icons::upload(),
                [this, selectedItems]()
                {
                    onTransferAsArchive(selectedItems);
                },
                !hasItems || archiveTransferDialog_ == nullptr
            )
        );
        items.push_back(Snc::PopupMenu::separator());
    }

    if (singleItem && selectedItems.front().isDirectoryLike() && selectedItems.front().path.filename() != "..")
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

    const auto remainingItems = std::vector<NuiFileExplorer::ContextMenuItem>{
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "open"),
            Ui5Icons::open_folder(),
            [this, selectedItems]()
            {
                onOpen(selectedItems.front(), false);
            },
            !singleItem || !openerCaps_.canOpenFile
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWith"),
            Ui5Icons::open_command_field(),
            [this, selectedItems]()
            {
                onOpen(selectedItems.front(), true);
            },
            !singleItem || !openerCaps_.canOpenFile
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openInFileManager"),
            Ui5Icons::show(),
            [this, selectedItems]()
            {
                onOpenInFileManager(selectedItems.front());
            },
            !singleItem || !openerCaps_.canOpenInFileManager
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

    items.insert(items.end(), remainingItems.begin(), remainingItems.end());
    return items;
}

void LocalSideModel::onOpen(NuiFileExplorer::Item const& item, bool openWith)
{
    Log::info("Open item requested: {}. Open with: {}", item.path.generic_string(), openWith);
    auto path = localPathOf(item);
    if (!path)
        return showPathRefused(language->get("localSideModel", "failedToOpenFile"), path.error());

    FileExplorer::openLocalFile(*confirmDialog_, *path, openWith);
}

void LocalSideModel::onOpenInFileManager(NuiFileExplorer::Item const& item)
{
    auto path = localPathOf(item);
    if (!path)
        return showPathRefused(language->get("localSideModel", "openInFileManagerFailed"), path.error());
    const auto absolutePath = path->toUtf8();
    Log::info("Open in file manager requested: {}", absolutePath);

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::openInFileManager",
        [this](Nui::val val)
        {
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::openInFileManager");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "openInFileManagerFailed"),
                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = language->translate(val["error"].as<std::string>());
                Log::error("Failed to open in file manager: {}", error);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "openInFileManagerFailed"),
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }
        },
        absolutePath
    );
}

void LocalSideModel::setRemoteModel(RemoteSideModel* model)
{
    remoteModel_ = model;
}

std::expected<Paths::LocalPath, Paths::PathError> LocalSideModel::currentLocalPath() const
{
    return Paths::LocalPath::fromUtf8(listedPath_.generic_string());
}

std::expected<Paths::LocalPath, Paths::PathError> LocalSideModel::localPathOf(NuiFileExplorer::Item const& item) const
{
    if (item.isParentLink)
        return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::DotOrDotDot, .input = ".."});
    return Paths::LocalPathSegment::make(item.name)
        .and_then(
            [&item](Paths::LocalPathSegment const&)
            {
                return Paths::LocalPath::fromUtf8(item.fullPath.generic_string());
            }
        );
}

bool LocalSideModel::isComplete() const
{
    return SideModel::isComplete();
}

namespace
{
    /** @brief Same mapping as RemoteSideModel — frontend-local ArchiveCodec →
     *         raw TarArchive::Compression int value used by the backend wire. */
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

void LocalSideModel::onTransferAsArchive(std::vector<NuiFileExplorer::Item> const& items)
{
    CHECK_COMPLETE();

    if (items.empty())
    {
        Log::error("No items selected for archive upload");
        return;
    }
    if (archiveTransferDialog_ == nullptr)
    {
        Log::error("ArchiveTransferDialog is not wired; cannot open archive upload dialog");
        return;
    }
    if (!remoteModel_)
    {
        Log::error("Cannot upload as archive: remote model is not set");
        return;
    }

    // Files go straight in; directories are flattened on the backend via
    // std::filesystem::recursive_directory_iterator inside the upload op's
    // prepareInStrand step.
    std::vector<Paths::LocalPath> localPaths;
    localPaths.reserve(items.size());
    for (auto const& item : items)
    {
        auto localPath = localPathOf(item);
        if (!localPath)
            return showPathRefused(language->get("localSideModel", "archiveUploadFailed"), localPath.error());
        localPaths.push_back(*std::move(localPath));
    }

    const auto defaultStem =
        items.size() == 1 ? items.front().path.filename().generic_string() : language->get("fileExplorer", "archiveDefaultName");

    archiveTransferDialog_->open({
        .headerText = language->get("fileExplorer", "contextMenu", "uploadAsArchive"),
        .initialFileStem = defaultStem,
        .initialCodec = ArchiveCodec::Gzip,
        .initialCompressionLevel = 5,
        .onConfirm = [this, paths = std::move(localPaths)](std::optional<ArchiveTransferResult> const& result) mutable
        {
            if (!result)
            {
                Log::info("Upload as Archive: user cancelled");
                return;
            }
            if (!remoteModel_)
                return;

            const std::string filename = result->fileStem + ".tar" + archiveCodecExtension(result->codec);
            auto archivePath = remoteModel_->currentRemotePath().and_then(
                [&filename](Paths::RemotePath const& directory)
                {
                    return Paths::PathSegment::make(filename).transform(
                        [&directory](Paths::PathSegment const& segment)
                        {
                            return directory / segment;
                        }
                    );
                }
            );
            if (!archivePath)
                return showPathRefused(language->get("localSideModel", "archiveUploadFailed"), archivePath.error());
            const auto remotePath = *std::move(archivePath);

            operationQueue_->enqueueArchiveUpload(
                std::move(paths),
                remotePath,
                backendCompressionCode(result->codec),
                result->compressionLevel,
                /*mayOverwrite=*/false,
                SharedData::OperationMode::Queued,
                [this, remotePath](std::optional<Ids::OperationId> const& opId, std::string const& info)
                {
                    if (!opId)
                    {
                        Log::error("Archive upload failed to enqueue: {}", info);
                        confirmDialog_->open({
                            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                            .headerText = language->get("localSideModel", "archiveUploadFailed"),
                            .text = info,
                            .buttons = ConfirmDialog::Button::Ok,
                        });
                        return;
                    }
                    Log::info("Archive upload queued as {} (destination {})", opId->value(), remotePath.value());
                }
            );
        },
    });
}

void LocalSideModel::generatePathBoxSuggestions(
    std::filesystem::path const& path,
    int maxSuggestions,
    std::function<void(std::vector<std::filesystem::path> const&)> onResultsAvailable
)
{
    CHECK_COMPLETE();
    pathSuggestionCache_.generateSuggestions(
        path,
        maxSuggestions,
        [onResultsAvailable](std::vector<std::filesystem::path> const& suggestions)
        {
            onResultsAvailable(suggestions);
        }
    );
}