#include <backend/dropped_entry.hpp>

SharedData::DirectoryEntry leanDirectoryEntryFromPath(std::filesystem::path const& path)
{
    using FileType = SharedData::DirectoryEntry::FileType;

    return SharedData::DirectoryEntry{
        .path = path,
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
}
