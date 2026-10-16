#include <frontend/file_explorer/local_side_model.hpp>
#include <frontend/file_explorer/open_local_file.hpp>
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

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/val_conversion.hpp>
#include <nui/rpc.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <iterator>
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

              Nui::RpcClient::callWithBackChannel(
                  "RpcFilesystem::listFiles",
                  [dirPath, onResultsAvailable = std::move(onResultsAvailable)](Nui::val val)
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
                              listing.emplace_back(dirPath / file["name"].as<std::string>());
                          }
                      }

                      onResultsAvailable(listing);
                  },
                  args
              );
          },
          0.,
      }
{
    setFavorites(uiOptions_.localFavorites);
    probeOpenerCapabilities();
}

void LocalSideModel::probeOpenerCapabilities()
{
    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::openerCapabilities",
        lifetime_.guarded(
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
        )
    );
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
        std::ranges::transform(
            places,
            std::back_inserter(entries),
            [](LocalPlace const& place)
            {
                return LocalSideModel::PlaceEntry{
                    .icon = iconForPlaceKind(place.kind),
                    .name = placeDisplayName(place.kind, place.name),
                    .path = place.path,
                };
            }
        );
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
                const auto length = drives["length"].as<int>();
                entries.reserve(static_cast<std::size_t>(length));
                for (int index = 0; index < length; ++index)
                {
                    entries.push_back({
                        .icon = Ui5Icons::database(),
                        .name = drives[index]["name"].as<std::string>(),
                        .path = drives[index]["path"].as<std::string>(),
                    });
                }
                callback(std::move(entries));
            }
        );
        return;
    }

    callback({});
}

void LocalSideModel::onActivateItem(NuiFileExplorer::Item const& item)
{
    if (!item.isDirectoryLike())
    {
        if (openerCaps_.canOpenFile)
            onOpen(item, false);
        return;
    }

    Log::debug("Local onActivateItem: {}.", item.displayName());
    navigateInto(item);
}
void LocalSideModel::onNewItem(NuiFileExplorer::Item::Type type)
{
    promptNewItem<Paths::LocalPath>(
        type,
        [this, type](Paths::LocalPath const& path)
        {
            if (type == NuiFileExplorer::Item::Type::Directory)
                return createItem(
                    "RpcFilesystem::createDirectory",
                    "path",
                    path,
                    language->get("localSideModel", "createDirectoryFailedTitle")
                );
            createItem(
                "RpcFilesystem::createFile", "filePath", path, language->get("localSideModel", "createFileFailedTitle")
            );
        }
    );
}

void LocalSideModel::createItem(
    char const* function,
    char const* pathArgument,
    Paths::LocalPath const& path,
    std::string failedTitle
)
{
    Nui::val args = Nui::val::object();
    args.set(pathArgument, path.toUtf8());

    Nui::RpcClient::callWithBackChannel(
        function,
        lifetime_.guarded(
            [this, function, failedTitle = std::move(failedTitle)](Nui::val val)
            {
                const auto error = [&val]() -> std::optional<std::string>
                {
                    if (!val.hasOwnProperty("success"))
                        return language->get("localSideModel", "invalidResponseFromBackend");
                    if (val["success"].as<bool>())
                        return std::nullopt;
                    return language->translate(val["error"].as<std::string>());
                }();
                if (!error)
                    return navigateTo(currentPath_.value());

                Log::error("{} failed: {}", function, *error);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = failedTitle,
                    .text = *error,
                    .buttons = ConfirmDialog::Button::Ok,
                });
            }
        ),
        args
    );
}

void LocalSideModel::onDelete(std::vector<NuiFileExplorer::Item> const& items)
{
    if (items.empty())
        return;

    const bool anyDirectory = std::ranges::any_of(
        items,
        [](NuiFileExplorer::Item const& item)
        {
            return item.type == NuiFileExplorer::Item::Type::Directory;
        }
    );
    // Local directories go with their content in one step, unlike remote ones there is no second question.
    const auto text = anyDirectory ? fmt::format(
                                         "{}\n{}",
                                         language->get("localSideModel", "deleteSelectedConfirm"),
                                         language->get("localSideModel", "deleteDirectoriesWithContent")
                                     )
                                   : language->get("localSideModel", "deleteSelectedConfirm");

    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
        .headerText = language->get("localSideModel", "deleteLocalItemsQuestion"),
        .text = text,
        .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
        .focusButton = ConfirmDialog::Button::No,
        .listItems = pathList(items),
        .onClose = lifetime_.guarded(
            [this, items, anyDirectory](std::optional<ConfirmDialog::Button> button)
            {
                if (!button || *button != ConfirmDialog::Button::Yes)
                {
                    Log::info("Delete items cancelled");
                    return;
                }

                std::vector<Paths::LocalPath> targets;
                targets.reserve(items.size());
                for (const auto& item : items)
                {
                    auto path = localPathOf(item);
                    if (!path)
                        return showPathRefused(language->get("localSideModel", "deleteFilesFailedTitle"), path.error());
                    targets.push_back(*std::move(path));
                }

                // Deleting a directory can take long, so it runs in the local operation queue with a card of its own.
                if (!anyDirectory)
                    return deleteFilesDirectly(targets);
                if (!operationQueue_)
                {
                    Log::warn("Cannot queue the delete of local items: no operation queue is set");
                    return showDeleteFailed(language->get("operationQueue", "unknownError"));
                }
                operationQueue_->enqueueLocalDelete(
                    targets,
                    lifetime_.guarded(
                        [this](std::optional<std::string> const& error)
                        {
                            if (!error)
                                return;
                            const auto reason = language->translate(*error);
                            Log::error("Failed to queue the delete of local items: {}", reason);
                            showDeleteFailed(reason);
                        }
                    )
                );
            }
        ),
    });
}

void LocalSideModel::deleteFilesDirectly(std::vector<Paths::LocalPath> const& targets)
{
    Nui::val args = Nui::val::object();
    args.set("paths", Nui::val::array());
    for (auto const& target : targets)
        args["paths"].call<void>("push", target.toUtf8());

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::removeSome",
        lifetime_.guarded(
            [this](Nui::val val)
            {
                const auto error = [&val]() -> std::optional<std::string>
                {
                    if (!val.hasOwnProperty("success"))
                        return language->get("localSideModel", "invalidResponseFromBackend");
                    if (val["success"].as<bool>())
                        return std::nullopt;
                    return language->translate(val["error"].as<std::string>());
                }();
                if (error)
                {
                    Log::error("Failed to delete local items: {}", *error);
                    showDeleteFailed(*error);
                }
                else
                    Log::info("Local items deleted");
                // The listing shows what is gone, also after a failure that kept some items.
                navigateTo(currentPath_.value());
            }
        ),
        args
    );
}

void LocalSideModel::showDeleteFailed(std::string const& reason)
{
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("localSideModel", "deleteFilesFailedTitle"),
        .text = reason,
        .buttons = ConfirmDialog::Button::Ok,
    });
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
        [&subDir](Paths::RemotePath const& directory)
        {
            return FileExplorer::targetDirectoryOf(directory, subDir);
        }
    );
    if (!destinationDirectory)
        return refuse(destinationDirectory.error());
    auto candidates = transferCandidates<SharedData::BulkUploadEntry>(
        items,
        [this](NuiFileExplorer::Item const& item)
        {
            return localPathOf(item);
        },
        *destinationDirectory
    );
    if (!candidates)
        return refuse(candidates.error());

    confirmTransfer<SharedData::BulkUploadEntry>(
        *std::move(candidates),
        {
            .headerText = language->get("localSideModel", "uploadConfirmTitle"),
            .singleText = language->get("localSideModel", "uploadSingleConfirmText"),
            .multipleText = language->get("localSideModel", "uploadMultipleConfirmText"),
            .destinationDirectory = destinationDirectory->value(),
            .overwriteHeaderText = language->get("localSideModel", "fileAlreadyExistsOverwriteHeader"),
            .overwriteText = language->get("localSideModel", "fileAlreadyExistsOverwrite"),
        },
        lifetime_.guarded(
            [this](std::vector<SharedData::BulkUploadEntry> accepted)
            {
                enqueueUploads(std::move(accepted));
            }
        )
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
            .onClose = lifetime_.guarded(
                [this, items, subDir](std::optional<ConfirmDialog::Button>)
                {
                    onDropExternal(items, subDir, false);
                }
            ),
        });
    }
    else
        onTransfer(items, subDir);
}

void LocalSideModel::onRename(NuiFileExplorer::Item const& item)
{
    Log::info("Rename item requested: {}", item.name);
    // The item's own path: the explorer may already show another directory.
    promptRename(item, localPathOf(item));
}

namespace
{
    std::optional<Nui::val> presentField(Nui::val const& source, char const* key)
    {
        if (!source.hasOwnProperty(key))
            return std::nullopt;
        auto value = source[key];
        if (value.isNull() || value.isUndefined())
            return std::nullopt;
        return value;
    }

    template <typename ValueT>
    ValueT fieldOrDefault(Nui::val const& source, char const* key)
    {
        return presentField(source, key)
            .transform(
                [](Nui::val const& value)
                {
                    return value.as<ValueT>();
                }
            )
            .value_or(ValueT{});
    }

    /**
     * @brief The entry the backend describes in @p source, with the entry a symlink resolves to when @p source
     *        carries it.
     */
    SharedData::DirectoryEntry directoryEntryOf(Nui::val const& source)
    {
        SharedData::DirectoryEntry entry{
            .name = source["name"].as<std::string>(),
            .type = SharedData::fileTypeFromStdFilesystemType(
                static_cast<std::filesystem::file_type>(source["type"].as<int>())
            ),
            .size = fieldOrDefault<std::uint64_t>(source, "size"),
            .uid = fieldOrDefault<std::uint32_t>(source, "uid"),
            .gid = fieldOrDefault<std::uint32_t>(source, "gid"),
            .owner = fieldOrDefault<std::string>(source, "owner"),
            .group = fieldOrDefault<std::string>(source, "group"),
            .permissions = static_cast<std::filesystem::perms>(fieldOrDefault<std::uint32_t>(source, "permissions")),
            .atime = fieldOrDefault<std::uint64_t>(source, "atime"),
            .atimeNsec = fieldOrDefault<std::uint32_t>(source, "atimeNsec"),
            .createTime = fieldOrDefault<std::uint64_t>(source, "createTime"),
            .createTimeNsec = fieldOrDefault<std::uint32_t>(source, "createTimeNsec"),
            .mtime = fieldOrDefault<std::uint64_t>(source, "mtime"),
            .mtimeNsec = fieldOrDefault<std::uint32_t>(source, "mtimeNsec"),
            .acl = fieldOrDefault<std::string>(source, "acl"),
        };
        if (const auto valid = presentField(source, "nameIsValidUtf8"); valid)
            entry.nameIsValidUtf8 = valid->as<bool>();
        if (const auto link = presentField(source, "linkTarget"); link)
            entry.linkTarget = link->as<std::string>();
        if (const auto target = presentField(source, "resolvedTarget"); target && entry.isSymlink())
            entry.resolvedTarget = std::make_shared<SharedData::DirectoryEntry>(directoryEntryOf(*target));
        return entry;
    }
}

void LocalSideModel::onProperties(NuiFileExplorer::Item const& item)
{
    // The listing carries fewer fields than the dialog shows, the full entry is asked for here.
    auto fallback = static_cast<SharedData::DirectoryEntry>(item);
    const auto path = localPathOf(item);
    if (!path)
    {
        Log::warn("Properties of '{}' use the listed entry: {}", item.name, path.error().toString());
        filePropertyDialog_->open(fallback, std::string{item.displayName()});
        return;
    }
    Nui::val args = Nui::val::object();
    args.set("path", path->toUtf8());

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::properties",
        lifetime_.guarded(
            [this, fallback = std::move(fallback), displayPath = path->toUtf8()](Nui::val val) mutable
            {
                const auto useFallback = [this, &fallback, &displayPath]()
                {
                    filePropertyDialog_->open(fallback, displayPath);
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

                const auto full = directoryEntryOf(val["entry"]);

                std::string resolvedPath{};
                if (val.hasOwnProperty("resolvedPath") && val["resolvedPath"].isString())
                    resolvedPath = val["resolvedPath"].as<std::string>();
                filePropertyDialog_->open(full, displayPath, std::move(resolvedPath));
            }
        ),
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

std::optional<std::filesystem::path> LocalSideModel::parentOf(std::filesystem::path const& directory) const
{
    return FileExplorer::parentOf<Paths::LocalPath>(directory.generic_string());
}

void LocalSideModel::navigateTo(std::filesystem::path const& path)
{
    const auto target = FileExplorer::navigationTarget<Paths::LocalPath>(path.generic_string());
    Log::info("Local navigating to: {}", target);
    preNavigatePath_ = currentPath_.value();
    currentPath_ = std::filesystem::path{target};

    Nui::val args = Nui::val::object();
    args.set("path", currentPath_.value().generic_string());

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::listFiles",
        lifetime_.guarded(
            [this, directory = currentPath_.value()](Nui::val val)
            {
                if (!val.hasOwnProperty("success"))
                    return onDirectoryListing(
                        directory, std::nullopt, language->get("localSideModel", "invalidResponseFromBackend")
                    );
                if (!val["success"].as<bool>())
                    return onDirectoryListing(
                        directory, std::nullopt, language->translate(val["error"].as<std::string>())
                    );

                std::vector<SharedData::DirectoryEntry> directoryEntries;
                const auto files = val["files"];
                const auto fileCount = files["length"].as<int>();
                directoryEntries.reserve(static_cast<std::size_t>(fileCount));
                for (int index = 0; index < fileCount; ++index)
                    directoryEntries.push_back(directoryEntryOf(files[index]));

                onDirectoryListing(directory, std::move(directoryEntries));
            }
        ),
        args
    );
}

void LocalSideModel::enqueueUploads(std::vector<SharedData::BulkUploadEntry> accepted)
{
    // One file shows as a single transfer card with progress and resume, directories need the scan of a bulk.
    if (accepted.size() == 1 && !accepted.front().isDirectory)
        return enqueueSingleUpload(accepted.front(), accepted.front().mayOverwrite, true);
    operationQueue_->enqueueBulkUpload(
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
                Log::error("Bulk upload failed: {}", info);
                confirmDialog_->open({
                    .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                    .headerText = language->get("localSideModel", "uploadFailed"),
                    .text = info,
                    .buttons = ConfirmDialog::Button::Ok,
                });
            }
        )
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
        lifetime_.guarded(
            [this, source = entry.source](std::optional<Ids::OperationId> const& opId, std::string const& info)
            {
                if (!opId)
                {
                    Log::error("Failed to create upload operation: {}", info);
                    confirmDialog_->open({
                        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
                        .headerText = "",
                        .text = fmt::format(
                            fmt::runtime(language->get("localSideModel", "failedToEnqueueUpload")),
                            source.toUtf8(),
                            info
                        ),
                        .buttons = ConfirmDialog::Button::Ok,
                    });
                    return;
                }
                Log::info("Upload operation created with id: {}", opId->value());
            }
        ),
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

    std::ranges::move(favoriteMenuItems(selectedItems), std::back_inserter(items));

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
    Log::info("Open item requested: {}. Open with: {}", item.name, openWith);
    const auto path = localPathOf(item);
    if (!path)
        return showPathRefused(language->get("localSideModel", "failedToOpenFile"), path.error());

    FileExplorer::openLocalFile(lifetime_, *confirmDialog_, *path, openWith);
}

void LocalSideModel::onOpenInFileManager(NuiFileExplorer::Item const& item)
{
    const auto path = localPathOf(item);
    if (!path)
        return showPathRefused(language->get("localSideModel", "openInFileManagerFailed"), path.error());
    const auto absolutePath = path->toUtf8();
    Log::info("Open in file manager requested: {}", absolutePath);

    Nui::RpcClient::callWithBackChannel(
        "RpcFilesystem::openInFileManager",
        lifetime_.guarded(
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
            }
        ),
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

std::expected<std::string, Paths::PathError> LocalSideModel::itemPathText(NuiFileExplorer::Item const& item) const
{
    return localPathOf(item).transform(
        [](Paths::LocalPath const& path)
        {
            return path.toUtf8();
        }
    );
}

std::expected<Paths::LocalPath, Paths::PathError> LocalSideModel::localPathOf(NuiFileExplorer::Item const& item) const
{
    if (item.isParentLink)
        return std::unexpected(Paths::PathError{.reason = Paths::PathRejection::DotOrDotDot, .input = ".."});
    return item.nameSegment()
        .and_then(
            [](Paths::PathSegment const& segment)
            {
                return Paths::LocalPathSegment::make(segment);
            }
        )
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
        items.size() == 1 ? items.front().name : language->get("fileExplorer", "archiveDefaultName");

    archiveTransferDialog_->open({
        .headerText = language->get("fileExplorer", "contextMenu", "uploadAsArchive"),
        .initialFileStem = defaultStem,
        .initialCodec = ArchiveCodec::Gzip,
        .initialCompressionLevel = 5,
        .onConfirm = lifetime_.guarded(
            [this, paths = std::move(localPaths)](std::optional<ArchiveTransferResult> const& result) mutable
            {
                if (!result)
                {
                    Log::info("Upload as Archive: user cancelled");
                    return;
                }
                if (!remoteModel_)
                    return;

                const auto filename = fmt::format("{}.tar{}", result->fileStem, archiveCodecExtension(result->codec));
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
                    lifetime_.guarded(
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
                            Log::info(
                                "Archive upload queued as {} (destination {})", opId->value(), remotePath.value()
                            );
                        }
                    )
                );
            }
        ),
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