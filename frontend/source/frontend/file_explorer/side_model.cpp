#include <frontend/file_explorer/side_model.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>
#include <script-nui-components/popup_menu.hpp>

#include <nui/event_system/event_context.hpp>

#include <ui5-sap-icons/icons/add-favorite.hpp>
#include <ui5-sap-icons/icons/unfavorite.hpp>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_set>

void SideModel::operationQueue(OperationQueue* operationQueue)
{
    operationQueue_ = operationQueue;
}
OperationQueue* SideModel::operationQueue()
{
    return operationQueue_;
}

void SideModel::engine(std::shared_ptr<FileEngine> fileEngine)
{
    fileEngine_ = std::move(fileEngine);
}

std::shared_ptr<FileEngine> SideModel::engine()
{
    return fileEngine_;
}

void SideModel::setOnFavoritesChanged(std::function<void(std::vector<std::string>)> callback)
{
    onFavoritesChanged_ = std::move(callback);
}

std::shared_ptr<Nui::Observed<std::vector<std::filesystem::path>>> SideModel::favorites() const
{
    return favorites_;
}

void SideModel::setFavorites(std::vector<std::string> const& favorites)
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(favorites.size());
    std::ranges::transform(
        favorites,
        std::back_inserter(paths),
        [](std::string const& favorite)
        {
            return std::filesystem::path{favorite};
        }
    );
    *favorites_ = std::move(paths);
}

void SideModel::addFavorite(std::filesystem::path const& path)
{
    if (std::ranges::contains(favorites_->value(), path))
        return;
    favorites_->push_back(path);
    notifyFavoritesChanged();
}

void SideModel::removeFavorite(std::filesystem::path const& path)
{
    if (std::erase(favorites_->value(), path) == 0)
        return;
    favorites_->modifyNow();
    notifyFavoritesChanged();
}

void SideModel::notifyFavoritesChanged()
{
    if (!onFavoritesChanged_)
        return;
    std::vector<std::string> favorites;
    favorites.reserve(favorites_->value().size());
    std::ranges::transform(
        favorites_->value(),
        std::back_inserter(favorites),
        [](std::filesystem::path const& favorite)
        {
            return favorite.generic_string();
        }
    );
    onFavoritesChanged_(std::move(favorites));
}

std::vector<NuiFileExplorer::ContextMenuItem>
SideModel::favoriteMenuItems(std::vector<NuiFileExplorer::Item> const& selectedItems)
{
    if (selectedItems.size() != 1 || !selectedItems.front().isDirectoryLike())
        return {};
    const auto pathText = itemPathText(selectedItems.front());
    if (!pathText)
        return {};

    const auto path = std::filesystem::path{*pathText};
    const bool isFavorite = std::ranges::contains(favorites_->value(), path);
    return {
        ScriptNuiComponents::PopupMenu::item(
            language->get("fileExplorer", "contextMenu", isFavorite ? "removeFromFavorites" : "addToFavorites"),
            isFavorite ? Ui5Icons::unfavorite() : Ui5Icons::add_favorite(),
            lifetime_.guarded(
                [this, path, isFavorite]()
                {
                    if (isFavorite)
                        removeFavorite(path);
                    else
                        addFavorite(path);
                }
            )
        ),
        ScriptNuiComponents::PopupMenu::separator(),
    };
}

std::vector<ConfirmDialog::OpenOptions::ListElement>
SideModel::pathList(std::vector<NuiFileExplorer::Item> const& items) const
{
    std::vector<ConfirmDialog::OpenOptions::ListElement> list;
    list.reserve(items.size());
    std::ranges::transform(
        items,
        std::back_inserter(list),
        [this](NuiFileExplorer::Item const& item)
        {
            return ConfirmDialog::OpenOptions::ListElement{
                .text = itemPathText(item).value_or(item.fullPath.generic_string()),
            };
        }
    );
    return list;
}

void SideModel::navigateInto(NuiFileExplorer::Item const& item)
{
    if (item.isParentLink)
        return navigateTo(item.fullPath);
    const auto path = itemPathText(item);
    if (!path)
        return showPathRefused(language->get("sideModel", "failedToListDirectory"), path.error());
    navigateTo(std::filesystem::path{*path});
}

void SideModel::showInvalidItemName(Paths::PathError const& error)
{
    Log::error("Refusing the name {}", error.toString());
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("fileExplorer", "invalidItemNameHeader"),
        .text = fmt::format(
            fmt::runtime(language->get("fileExplorer", "invalidItemName")),
            error.input,
            language->get("fileExplorer", "nameRejection", FileExplorer::nameRejectionKey(error.reason))
        ),
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void SideModel::showPathRefused(std::string const& headerText, Paths::PathError const& error)
{
    Log::error("{}: {}", headerText, error.toString());
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = headerText,
        .text = error.toString(),
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void SideModel::showExistenceCheckFailed(std::string const& reason)
{
    Log::error("Transfer stopped, the existence check failed: {}", reason);
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("fileExplorer", "existenceCheckFailedHeader"),
        .text = fmt::format(fmt::runtime(language->get("fileExplorer", "existenceCheckFailed")), reason),
        .buttons = ConfirmDialog::Button::Ok,
    });
}

void SideModel::confirmReplaceOnRename(std::string const& name, std::function<void()> onReplace)
{
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Warning,
        .headerText = language->get("fileExplorer", "renameTargetExistsHeader"),
        .text = fmt::format(fmt::runtime(language->get("fileExplorer", "renameTargetExists")), name),
        .buttons = ConfirmDialog::Button::Yes | ConfirmDialog::Button::No,
        .focusButton = ConfirmDialog::Button::No,
        .onClose =
            [onReplace = std::move(onReplace)](std::optional<ConfirmDialog::Button> button)
        {
            if (button == ConfirmDialog::Button::Yes)
                onReplace();
        },
    });
}

FileExplorer::OverwriteAnswer SideModel::overwriteAnswerOf(std::optional<ConfirmDialog::Button> button)
{
    if (button == ConfirmDialog::Button::Yes)
        return FileExplorer::OverwriteAnswer::Yes;
    if (button == ConfirmDialog::Button::No)
        return FileExplorer::OverwriteAnswer::No;
    if (button == ConfirmDialog::Button::All)
        return FileExplorer::OverwriteAnswer::All;
    if (button == ConfirmDialog::Button::None)
        return FileExplorer::OverwriteAnswer::None;
    return FileExplorer::OverwriteAnswer::Cancel;
}

bool SideModel::isComplete() const
{
    return operationQueue_ != nullptr && fileEngine_ != nullptr;
}

void SideModel::setItemUpdateFunction(std::function<void(bool, bool)> doUpdate)
{
    refreshCallback_ = std::move(doUpdate);
}

void SideModel::setPendingDeleteFunction(
    std::function<void(std::vector<std::filesystem::path> const&)> markPendingDelete
)
{
    pendingDeleteCallback_ = std::move(markPendingDelete);
}

void SideModel::markPendingDelete(std::vector<std::string> const& targets)
{
    if (!pendingDeleteCallback_ || targets.empty())
        return;

    const std::unordered_set<std::string_view> targetSet{targets.begin(), targets.end()};
    std::vector<std::filesystem::path> fullPaths;
    for (auto const& item : items_)
    {
        const auto path = itemPathText(item);
        if (path && targetSet.contains(*path))
            fullPaths.push_back(item.fullPath);
    }
    pendingDeleteCallback_(fullPaths);
    Nui::globalEventContext.executeActiveEventsImmediately();
}
const std::vector<NuiFileExplorer::Item>& SideModel::items() const
{
    return items_;
}

std::string SideModel::iconOf(SharedData::DirectoryEntry const& entry) const
{
    if (entry.isDirectoryLike())
        return "nui://app.example/icons/folder.svg";
    if (entry.type == SharedData::DirectoryEntry::FileType::BlockDevice)
        return "nui://app.example/icons/hard-drive.svg";

    const auto extension = std::filesystem::path{entry.name}.extension().generic_string();
    if (const auto icon = uiOptions_.fileGridExtensionIcons.find(extension);
        icon != uiOptions_.fileGridExtensionIcons.end())
        return fmt::format("nui://app.example/{}", icon->second);
    return "nui://app.example/icons/file.svg";
}

void SideModel::revertNavigation()
{
    if (currentPath_.value() != listedPath_)
    {
        currentPath_ = listedPath_;
        currentPath_.modifyNow();
    }
    // Showing the listed items again, sorted and with their selection, also ends the side's loading state.
    if (refreshCallback_)
        refreshCallback_(true, true);
}

void SideModel::onDirectoryListing(
    std::filesystem::path const& directory,
    std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries,
    std::string const& reason
)
{
    if (directory != currentPath_.value())
    {
        Log::debug("Dropping the listing of '{}', the side moved on.", directory.generic_string());
        return;
    }

    if (!directoryEntries)
    {
        Log::error("Failed to list directory: {}", reason);
        const auto path = currentPath_->generic_string();
        auto text = reason.empty()
            ? fmt::format(fmt::runtime(language->get("sideModel", "failedToListDirectoryText")), path)
            : fmt::format(fmt::runtime(language->get("sideModel", "failedToListDirectoryReasonText")), path, reason);
        confirmDialog_->open({
            .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
            .headerText = language->get("sideModel", "failedToListDirectory"),
            .text = std::move(text),
            .buttons = ConfirmDialog::Button::Ok,
        });
        return revertNavigation();
    }

    // Servers send "." and "..", the parent link below stands for the parent instead.
    std::erase_if(
        *directoryEntries,
        [](auto const& entry)
        {
            return entry.name == "." || entry.name == "..";
        }
    );
    listedPath_ = directory;

    std::vector<NuiFileExplorer::Item> items{};
    items.reserve(directoryEntries->size() + 1);
    if (auto parent = parentOf(directory); parent)
        items.push_back(NuiFileExplorer::Item::parentLink(*std::move(parent), "nui://app.example/icons/folder.svg"));
    std::ranges::transform(
        *directoryEntries,
        std::back_inserter(items),
        [this, &directory](SharedData::DirectoryEntry& entry)
        {
            auto fullPath = directory / entry.name;
            auto icon = iconOf(entry);
            auto item = NuiFileExplorer::Item{std::move(entry), std::move(icon)};
            item.fullPath = std::move(fullPath);
            return item;
        }
    );

    items_ = std::move(items);
    if (refreshCallback_)
    {
        refreshCallback_(true, reapplySelectionOnce_);
        reapplySelectionOnce_ = false;
    }
}