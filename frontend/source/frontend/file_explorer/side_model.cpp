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
    std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries,
    std::string const& reason
)
{
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

    std::erase_if(
        *directoryEntries,
        [](auto const& entry)
        {
            return entry.path.filename() == ".";
        }
    );

    for (auto& entry : *directoryEntries)
    {
        if (entry.fullPath.empty())
            entry.fullPath = *currentPath_ / entry.path;
    }

    std::vector<NuiFileExplorer::Item> items{};
    std::transform(
        begin(*directoryEntries),
        end(*directoryEntries),
        std::back_inserter(items),
        [this](auto const& entry)
        {
            return NuiFileExplorer::Item{
                entry,
                [&entry, this]() -> std::string
                {
                    const auto type = static_cast<NuiFileExplorer::Item::Type>(entry.type);
                    if (entry.isDirectoryLike())
                        return "nui://app.example/icons/folder.svg";
                    if (type == NuiFileExplorer::Item::Type::BlockDevice)
                        return "nui://app.example/icons/hard-drive.svg";

                    if (uiOptions_.fileGridExtensionIcons.contains(entry.path.extension().string()))
                    {
                        return "nui://app.example/" +
                            uiOptions_.fileGridExtensionIcons.at(entry.path.extension().string());
                    }

                    return "nui://app.example/icons/file.svg";
                }()
            };
        }
    );

    items_ = std::move(items);
    if (refreshCallback_)
    {
        refreshCallback_(true, reapplySelectionOnce_);
        reapplySelectionOnce_ = false;
    }
}