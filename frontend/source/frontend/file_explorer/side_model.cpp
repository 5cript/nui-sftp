#include <frontend/file_explorer/side_model.hpp>
#include <log/log.hpp>
#include <utility/language.hpp>

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

bool SideModel::isValidItemName(std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    return name.find_first_of(std::string_view{"/\\\0", 3}) == std::string_view::npos;
}

void SideModel::showInvalidItemName(std::string const& name)
{
    confirmDialog_->open({
        .styleVariant = ScriptNuiComponents::StyleVariant::Danger,
        .headerText = language->get("fileExplorer", "invalidItemNameHeader"),
        .text = fmt::format(fmt::runtime(language->get("fileExplorer", "invalidItemName")), name),
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

bool SideModel::isComplete() const
{
    return operationQueue_ != nullptr && fileEngine_ != nullptr;
}

void SideModel::setItemUpdateFunction(std::function<void(bool, bool)> doUpdate)
{
    refreshCallback_ = std::move(doUpdate);
}
const std::vector<NuiFileExplorer::Item>& SideModel::items() const
{
    return items_;
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
        // undo the navigation, an initial navigation has nowhere to go back to:
        if (preNavigatePath_.empty())
            currentPath_ = preNavigatePath_;
        else if (currentPath_.value() != preNavigatePath_)
        {
            currentPath_ = preNavigatePath_;
            navigateTo(currentPath_.value());
        }
        return;
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
    if (hasParent(directory))
        items.push_back(NuiFileExplorer::Item::parentLink(directory.parent_path(), "nui://app.example/icons/folder.svg"));
    std::transform(
        begin(*directoryEntries),
        end(*directoryEntries),
        std::back_inserter(items),
        [this, &directory](auto const& entry)
        {
            auto item = NuiFileExplorer::Item{
                entry,
                [&entry, this]() -> std::string
                {
                    const auto type = static_cast<NuiFileExplorer::Item::Type>(entry.type);
                    if (entry.isDirectoryLike())
                        return "nui://app.example/icons/folder.svg";
                    if (type == NuiFileExplorer::Item::Type::BlockDevice)
                        return "nui://app.example/icons/hard-drive.svg";

                    const auto extension = std::filesystem::path{entry.name}.extension().string();
                    if (uiOptions_.fileGridExtensionIcons.contains(extension))
                        return "nui://app.example/" + uiOptions_.fileGridExtensionIcons.at(extension);

                    return "nui://app.example/icons/file.svg";
                }()
            };
            item.fullPath = directory / entry.name;
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