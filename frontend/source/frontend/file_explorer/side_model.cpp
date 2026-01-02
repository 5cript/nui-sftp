#include <frontend/file_explorer/side_model.hpp>
#include <log/log.hpp>

void SideModel::operationQueue(OperationQueue* operationQueue)
{
    operationQueue_ = operationQueue;
}
OperationQueue* SideModel::operationQueue()
{
    return operationQueue_;
}

void SideModel::engine(std::unique_ptr<FileEngine> fileEngine)
{
    fileEngine_ = std::move(fileEngine);
}

FileEngine* SideModel::engine()
{
    return fileEngine_.get();
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

void SideModel::onDirectoryListing(std::optional<std::vector<SharedData::DirectoryEntry>> directoryEntries)
{
    if (!directoryEntries)
    {
        Log::error("Failed to list directory");
        // undo the navigation:
        if (currentPath_.value() != preNavigatePath_)
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

    std::vector<NuiFileExplorer::Item> items{};
    std::transform(
        begin(*directoryEntries),
        end(*directoryEntries),
        std::back_inserter(items),
        [this](auto const& entry)
        {
            return NuiFileExplorer::Item{
                .path = entry.path,
                .icon = [&entry, this]() -> std::string
                {
                    const auto type = static_cast<NuiFileExplorer::Item::Type>(entry.type);
                    if (type == NuiFileExplorer::Item::Type::Directory)
                        return "nui://app.example/icons/folder_main.png";
                    if (type == NuiFileExplorer::Item::Type::BlockDevice)
                        return "nui://app.example/icons/hard_drive.png";

                    if (uiOptions_.fileGridExtensionIcons.contains(entry.path.extension().string()))
                    {
                        return "nui://app.example/" +
                            uiOptions_.fileGridExtensionIcons.at(entry.path.extension().string());
                    }

                    return "nui://app.example/icons/file.png";
                }(),
                .type = static_cast<NuiFileExplorer::Item::Type>(entry.type),
                .permissions = entry.permissions,
                .ownerId = entry.uid,
                .groupId = entry.gid,
                .owner = entry.owner,
                .group = entry.group,
                .atime = entry.atime,
                .size = entry.size,
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