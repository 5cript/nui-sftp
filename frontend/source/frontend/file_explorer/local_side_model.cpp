#include <frontend/file_explorer/local_side_model.hpp>
#include <nui-file-explorer/preprocessor.hpp>
#include <script-nui-components/popup_menu.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

#include <ui5-sap-icons/icons/home.hpp>
#include <ui5-sap-icons/icons/desktop-mobile.hpp>
#include <ui5-sap-icons/icons/download.hpp>
#include <ui5-sap-icons/icons/documents.hpp>
#include <ui5-sap-icons/icons/picture.hpp>
#include <ui5-sap-icons/icons/video.hpp>
#include <ui5-sap-icons/icons/folder.hpp>
#include <ui5-sap-icons/icons/database.hpp>
#include <ui5-sap-icons/icons/upload.hpp>
#include <ui5-sap-icons/icons/open-folder.hpp>
#include <ui5-sap-icons/icons/open-command-field.hpp>
#include <ui5-sap-icons/icons/delete.hpp>
#include <ui5-sap-icons/icons/edit.hpp>
#include <ui5-sap-icons/icons/detail-view.hpp>
#include <ui5-sap-icons/icons/add-favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>

#include <nui/frontend/api/console.hpp>
#include <nui/rpc.hpp>

#include <algorithm>

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

LocalSideModel::LocalSideModel(
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
                          const auto error = val["error"].as<std::string>();
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
{}

void LocalSideModel::setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback)
{
    onFavoritesChanged_ = std::move(callback);
}

// --- IPlacesProvider ---

void LocalSideModel::requestDefaultPlaces(std::function<void(std::vector<PlaceEntry>)> callback)
{
    Nui::RpcClient::callWithBackChannel(
        "NuiFileExplorer::DefaultPlaces::list",
        [callback = std::move(callback)](Nui::val val)
        {
            if (!val.hasOwnProperty("success") || !val["success"].as<bool>())
            {
                Log::error("DefaultPlaces::list failed");
                callback({});
                return;
            }
            std::vector<PlaceEntry> entries;
            const auto places = val["places"];
            const auto len = places["length"].as<int>();
            entries.reserve(len);
            for (int idx = 0; idx < len; ++idx)
            {
                auto const placeName = places[idx]["name"].as<std::string>();
                entries.push_back({
                    .icon = iconForPlaceName(placeName),
                    .name = placeName,
                    .path = places[idx]["path"].as<std::string>(),
                });
            }
            callback(std::move(entries));
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
            .onConfirm = [this, type](std::optional<std::string> const& name)
            {
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
                                const auto error = val["error"].as<std::string>();
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
                    args.set("path", (currentPath_.value() / *name).generic_string());

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
                                const auto error = val["error"].as<std::string>();
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
            .headerText = "Delete Items?",
            .text = "Are you sure you want to delete the selected items?",
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
                    args["paths"].call<void>("push", (*currentPath_ / item.path).generic_string());
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
                            const auto error = val["error"].as<std::string>();
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
            .headerText = "File Transfer Failed",
            .text = "Remote side is not available",
            .buttons = ConfirmDialog::Button::Ok,
        });
        return;
    }

    Log::info("Upload items requested: {}", items.size());
    for (const auto& item : items)
    {
        Log::debug("Item: {}", item.path.generic_string());
    }

    auto destinationDir = remoteModel_->currentPath().value();
    if (subDir)
        destinationDir /= *subDir;
    const auto sourceDir = currentPath_.value();

    const auto itemsSize = items.size();
    std::string confirmText = fmt::format(
        fmt::runtime(
            language->get("localSideModel", itemsSize > 1 ? "uploadMultipleConfirmText" : "uploadSingleConfirmText")
        ),
        itemsSize > 1 ? std::to_string(itemsSize) : items.front().path.filename().generic_string(),
        destinationDir.generic_string()
    );

    std::vector<ConfirmDialog::OpenOptions::ListElement> listItems;
    for (const auto& item : items)
    {
        listItems.push_back({item.path.generic_string(), ""});
    }

    confirmDialog_->open(
        {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
            .headerText = language->get("localSideModel", "uploadConfirmTitle"),
            .text = confirmText,
            .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
            .focusButton = ConfirmDialog::Button::Yes,
            .listItems = listItems,
            .onClose = [this, items, destinationDir, sourceDir](std::optional<ConfirmDialog::Button> button)
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Upload items cancelled");
                    return;
                }

                // pair <remote, local>
                std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> uploadItems;
                std::transform(
                    items.begin(),
                    items.end(),
                    std::back_inserter(uploadItems),
                    [destinationDir, sourceDir](auto const& item)
                    {
                        auto fullSourcePath = [&]()
                        {
                            // if the item has a slash in it, assume its a full path:
                            if (item.path.has_parent_path())
                                return item.path;
                            return sourceDir / item.path;
                        };
                        auto localItem = item;
                        auto remoteItem = item;
                        localItem.path = fullSourcePath();
                        localItem.fullPath = localItem.path;
                        remoteItem.path = destinationDir / item.path.filename();
                        remoteItem.fullPath = remoteItem.path;
                        return std::make_pair(remoteItem, localItem);
                    }
                );

                Log::info("Uploading items");
                uploadItemsConfirmed(uploadItems);
            }}
    );
}

void LocalSideModel::onDropExternal(
    std::vector<NuiFileExplorer::Item> const& items,
    std::optional<std::string> const& subDir,
    bool issueWebkitWarning
)
{
    if (issueWebkitWarning &&
        (STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkitgtk"s || STRINGIZE_EXPANDED(BROWSER_ENGINE) == "webkit"s))
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

void LocalSideModel::onRename(NuiFileExplorer::Item const& item)
{
    Log::info("Rename item requested: {}", item.path.generic_string());

    auto doRename = [item, this](std::string const& newName)
    {
        Nui::val args = Nui::val::object();
        args.set("oldPath", (*currentPath_ / item.path).generic_string());
        args.set("newPath", (*currentPath_ / newName).generic_string());

        Nui::RpcClient::callWithBackChannel(
            "RpcFilesystem::rename",
            [this](Nui::val val)
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

                const auto success = val["success"].as<bool>();
                if (!success)
                {
                    const auto error = val["error"].as<std::string>();
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
                // Refresh list:
                onRefresh();
            },
            args
        );
    };

    inputDialog_->open({
        .whatFor = "Rename",
        .prompt = "Enter the new name for " + item.path.filename().string(),
        .headerText = "Rename " + item.path.filename().string(),
        .isPassword = false,
        .onConfirm = [doRename](std::optional<std::string> const& name)
        {
            if (!name)
                return;

            Log::info("Renaming item to: {}", *name);
            doRename(*name);
        },
    });
}
void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    filePropertyDialog_->open(static_cast<SharedData::DirectoryEntry const&>(item));
}
void LocalSideModel::onError(std::string const& error)
{
    Log::error("File grid error (local side): {}", error);
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("localSideModel", "fileGridError"),
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
            }
        );
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
        [this](Nui::val val)
        {
            Log::debug("Received directory listing response");
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::properties");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToListFiles"),
                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
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
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToListFiles"),
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            std::vector<SharedData::DirectoryEntry> directoryEntries;
            const auto files = val["files"];
            for (const auto& file : files)
            {
                const auto fileType = SharedData::fileTypeFromStdFilesystemType(
                    static_cast<std::filesystem::file_type>(file["type"].as<int>())
                );
                directoryEntries.push_back(
                    SharedData::DirectoryEntry{
                        .path = file["path"].as<std::string>(),
                        .type = fileType,
                        .size = file["size"].as<std::uint64_t>(),
                        .uid = file["uid"].as<std::uint32_t>(),
                        .gid = file["gid"].as<std::uint32_t>(),
                        .owner = file["owner"].as<std::string>(),
                        .group = file["group"].as<std::string>(),
                        .permissions = static_cast<std::filesystem::perms>(file["permissions"].as<int>()),
                        .atime = file["atime"].as<std::uint64_t>(),
                        .atimeNsec = file["atimeNsec"].as<std::uint32_t>(),
                        .createTime = file["createTime"].as<std::uint64_t>(),
                        .createTimeNsec = file["createTimeNsec"].as<std::uint32_t>(),
                        .mtime = file["mtime"].as<std::uint64_t>(),
                        .mtimeNsec = file["mtimeNsec"].as<std::uint32_t>(),
                        .resolvedTarget = [&]() -> std::shared_ptr<SharedData::DirectoryEntry>
                        {
                            if (fileType != SharedData::FileType::Symlink)
                                return nullptr;
                            if (!file.hasOwnProperty("resolvedTarget") || file["resolvedTarget"].isNull() ||
                                file["resolvedTarget"].isUndefined())
                                return nullptr;
                            const auto target = file["resolvedTarget"];
                            return std::make_shared<SharedData::DirectoryEntry>(SharedData::DirectoryEntry{
                                .path = target["path"].as<std::string>(),
                                .type = SharedData::fileTypeFromStdFilesystemType(
                                    static_cast<std::filesystem::file_type>(target["type"].as<int>())
                                ),
                                .size = target["size"].as<std::uint64_t>(),
                                .uid = target["uid"].as<std::uint32_t>(),
                                .gid = target["gid"].as<std::uint32_t>(),
                                .owner = target["owner"].as<std::string>(),
                                .group = target["group"].as<std::string>(),
                                .permissions = static_cast<std::filesystem::perms>(target["permissions"].as<int>()),
                                .atime = target["atime"].as<std::uint64_t>(),
                                .atimeNsec = target["atimeNsec"].as<std::uint32_t>(),
                                .createTime = target["createTime"].as<std::uint64_t>(),
                                .createTimeNsec = target["createTimeNsec"].as<std::uint32_t>(),
                                .mtime = target["mtime"].as<std::uint64_t>(),
                                .mtimeNsec = target["mtimeNsec"].as<std::uint32_t>(),
                            });
                        }(),
                    }
                );
            }

            onDirectoryListing(directoryEntries);
        },
        args
    );
}

void LocalSideModel::uploadItemsConfirmed(
    std::vector<std::pair<NuiFileExplorer::Item, NuiFileExplorer::Item>> uploadItems,
    std::size_t index,
    bool overwriteNever,
    bool overwriteAlways
)
{
    if (index == uploadItems.size())
        return;

    if (overwriteAlways)
    {
        for (; index < uploadItems.size(); ++index)
            enqueueSingleUpload(
                uploadItems[index].first, uploadItems[index].second, overwriteAlways, index + 1 == uploadItems.size()
            );
        return;
    }

    fileEngine_->stat(
        uploadItems[index].first.path,
        [this, uploadItems = std::move(uploadItems), index, overwriteNever, overwriteAlways](
            std::optional<std::pair<bool, SharedData::DirectoryEntry>> const& entry, std::string const& info
        ) mutable
        {
            if (!entry)
            {
                Log::error("Failed to stat file: {}", uploadItems[index].first.path.generic_string());
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "",
                    .text = fmt::format(
                        fmt::runtime(language->get("localSideModel", "failedToStatFile")),
                        uploadItems[index].first.path.generic_string(),
                        info
                    ),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            auto const& item = uploadItems[index];

            Log::debug(
                "File '{}' {} exist on remote side. OverwriteNever: {}, OverwriteAlways: {}",
                item.second.path.generic_string(),
                entry->first ? "does" : "does not",
                overwriteNever,
                overwriteAlways
            );

            if (entry->first && !overwriteNever)
            {
                Log::info(
                    "Uploading '{}' to '{}'.", item.first.path.generic_string(), item.second.path.generic_string()
                );
                confirmDialog_->open(
                    {.styleVariant = ScriptNuiComponents::StyleVariant::Primary,
                        .headerText = language->get("localSideModel", "fileAlreadyExistsOverwriteHeader"),
                        .text = language->get("localSideModel", "fileAlreadyExistsOverwrite"),
                        .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No | ConfirmDialog::Button::All |
                            ConfirmDialog::Button::None | ConfirmDialog::Button::Cancel,
                        .focusButton = ConfirmDialog::Button::No,
                        .listItems =
                            {{.text = item.second.path.generic_string(), .description = "File already exists"}},
                        .onClose = [this, uploadItems = std::move(uploadItems), index, overwriteNever, overwriteAlways](
                                       std::optional<ConfirmDialog::Button> button
                                   ) mutable
                        {
                            if (button && *button == ConfirmDialog::Button::Yes)
                            {
                                enqueueSingleUpload(
                                    uploadItems[index].first,
                                    uploadItems[index].second,
                                    true,
                                    index + 1 == uploadItems.size()
                                );
                                uploadItemsConfirmed(
                                    std::move(uploadItems), index + 1, overwriteNever, overwriteAlways
                                );
                            }
                            else if (button && *button == ConfirmDialog::Button::No)
                            {
                                Log::info(
                                    "Skipping upload of existing file: {}",
                                    uploadItems[index].second.path.generic_string()
                                );
                                uploadItemsConfirmed(
                                    std::move(uploadItems), index + 1, overwriteNever, overwriteAlways
                                );
                            }
                            else if (button && *button == ConfirmDialog::Button::All)
                            {
                                Log::info("Overwriting all existing files from now on.");
                                enqueueSingleUpload(
                                    uploadItems[index].first,
                                    uploadItems[index].second,
                                    true,
                                    index + 1 == uploadItems.size()
                                );
                                uploadItemsConfirmed(std::move(uploadItems), index + 1, overwriteNever, true);
                            }
                            else if (button && *button == ConfirmDialog::Button::None)
                            {
                                Log::info("Skipping all existing files from now on.");
                                uploadItemsConfirmed(std::move(uploadItems), index + 1, true, overwriteAlways);
                            }

                            Log::info(
                                "User cancelled upload of existing file: {}",
                                uploadItems[index].second.path.generic_string()
                            );
                        }}
                );
                return;
            }

            if (!entry->first)
                enqueueSingleUpload(item.first, item.second, false, index + 1 == uploadItems.size());
            uploadItemsConfirmed(std::move(uploadItems), index + 1, overwriteNever, overwriteAlways);
        }
    );
}

void LocalSideModel::enqueueSingleUpload(
    NuiFileExplorer::Item const& remoteItem,
    NuiFileExplorer::Item const& localItem,
    bool allowOverwrite,
    bool insertRefresh
)
{
    operationQueue_->enqueueUpload(
        remoteItem,
        localItem,
        [this, localItem](std::optional<Ids::OperationId> const& opId, std::string const& info)
        {
            if (!opId)
            {
                Log::error("Failed to create upload operation: {}", info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = "",
                    .text = fmt::format(
                        fmt::runtime(language->get("localSideModel", "failedToEnqueueUpload")),
                        localItem.path.generic_string(),
                        info
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
        items.push_back(Snc::PopupMenu::separator());
    }

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

    const auto remainingItems = std::vector<NuiFileExplorer::ContextMenuItem>{
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "open"),
            Ui5Icons::open_folder(),
            [this, selectedItems]()
            {
                onOpen(selectedItems.front(), false);
            },
            !singleItem
        ),
        Snc::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", "openWith"),
            Ui5Icons::open_command_field(),
            [this, selectedItems]()
            {
                onOpen(selectedItems.front(), true);
            },
            !singleItem
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

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::open",
        [this](Nui::val val)
        {
            if (!val.hasOwnProperty("success"))
            {
                Log::error("Invalid response from RpcFilesystem::open");
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToOpenFile"),
                    .text = language->get("localSideModel", "invalidResponseFromBackend"),
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            const auto success = val["success"].as<bool>();
            if (!success)
            {
                const auto error = val["error"].as<std::string>();
                Log::error("Failed to open file: {}", error);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "failedToOpenFile"),
                    .text = error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
                return;
            }

            Log::info("File opened successfully");
        },
        (*currentPath_ / item.path).generic_string(),
        openWith
    );
}

void LocalSideModel::setRemoteModel(SideModel* model)
{
    remoteModel_ = model;
}

bool LocalSideModel::isComplete() const
{
    return SideModel::isComplete();
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