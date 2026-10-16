#include <backend/dropped_entry.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/error_message.hpp>
#include <utility/link_safe_remove.hpp>

#include <system_error>

namespace
{
    /**
     * @brief The type of what lies at @p path itself: a link or junction is a symlink, never what it points to,
     *        like every upload from the explorer sees it.
     */
    SharedData::FileType droppedTypeOf(std::filesystem::path const& path, std::filesystem::file_status const& status)
    {
        using FileType = SharedData::FileType;
        std::error_code linkError{};
        if (std::filesystem::is_symlink(status) || Utility::isLinkOrJunction(path, linkError))
            return FileType::Symlink;
        if (std::filesystem::is_directory(status))
            return FileType::Directory;
        if (std::filesystem::is_regular_file(status))
            return FileType::Regular;
        if (std::filesystem::is_socket(status))
            return FileType::Socket;
        if (std::filesystem::is_character_file(status))
            return FileType::CharDevice;
        if (std::filesystem::is_block_file(status))
            return FileType::BlockDevice;
        if (std::filesystem::is_other(status))
            return FileType::Special;
        return FileType::Unknown;
    }
}

std::optional<SharedData::DroppedItem> droppedItemFromPath(std::filesystem::path const& path)
{
    auto localPath = Paths::LocalPath::make(path);
    if (!localPath)
    {
        Log::warn("Drop: '{}' is ignored: {}", Paths::nativeToUtf8(path), localPath.error().toString());
        return std::nullopt;
    }
    std::error_code statusError{};
    const auto status = std::filesystem::symlink_status(path, statusError);
    if (status.type() == std::filesystem::file_type::not_found || statusError)
    {
        Log::warn(
            "Drop: '{}' is ignored, it cannot be inspected: {}",
            localPath->toUtf8(),
            statusError ? Utility::errorMessage(statusError) : std::string{"not found"}
        );
        return std::nullopt;
    }
    const auto name = localPath->filename();
    auto entry = SharedData::DirectoryEntry{
        .name = name ? name->value() : std::string{},
        .type = droppedTypeOf(path, status),
    };
    return SharedData::DroppedItem{.path = *std::move(localPath), .entry = std::move(entry)};
}
