#include <backend/dropped_entry.hpp>

std::optional<SharedData::DroppedItem> droppedItemFromPath(std::filesystem::path const& path)
{
    using FileType = SharedData::DirectoryEntry::FileType;

    auto localPath = Paths::LocalPath::make(path);
    if (!localPath)
        return std::nullopt;
    const auto name = localPath->filename();
    auto entry = SharedData::DirectoryEntry{
        .name = name ? name->value() : std::string{},
        .type = [&path]()
        {
            std::error_code statusError;
            const auto status = std::filesystem::status(path, statusError);
            if (std::filesystem::is_directory(status))
                return FileType::Directory;
            else if (std::filesystem::is_regular_file(status))
                return FileType::Regular;
            else if (std::filesystem::is_symlink(status))
                return FileType::Symlink;
            else if (std::filesystem::is_socket(status))
                return FileType::Socket;
            else if (std::filesystem::is_character_file(status))
                return FileType::CharDevice;
            else if (std::filesystem::is_block_file(status))
                return FileType::BlockDevice;
            else if (std::filesystem::is_other(status))
                return FileType::Special;
            else
                return FileType::Unknown;
        }()
    };
    return SharedData::DroppedItem{.path = *std::move(localPath), .entry = std::move(entry)};
}
