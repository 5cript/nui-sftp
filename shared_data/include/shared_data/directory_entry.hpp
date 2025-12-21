#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace SharedData
{
    enum class FileType : std::uint8_t
    {
        Unknown = 0,
        Regular = 1,
        Directory = 2,
        Symlink = 3,
        Special = 4,
        Socket = 5,
        CharDevice = 6,
        BlockDevice = 7,
        Fifo = 8
    };

    inline FileType fileTypeFromStdFilesystemType(std::filesystem::file_type type)
    {
        using enum std::filesystem::file_type;
        switch (type)
        {
            case (regular):
                return FileType::Regular;
            case (directory):
                return FileType::Directory;
            case (symlink):
                return FileType::Symlink;
            case (block):
                return FileType::BlockDevice;
            case (character):
                return FileType::CharDevice;
            case (fifo):
                return FileType::Fifo;
            case (socket):
                return FileType::Socket;
            case (unknown):
                return FileType::Unknown;
            default:
                return FileType::Unknown;
        }
    }

    inline std::string fileTypeToString(FileType type)
    {
        switch (type)
        {
            case FileType::Regular:
                return "Regular File";
            case FileType::Directory:
                return "Directory";
            case FileType::Symlink:
                return "Symbolic Link";
            case FileType::Special:
                return "Special File";
            case FileType::Socket:
                return "Socket";
            case FileType::CharDevice:
                return "Character Device";
            case FileType::BlockDevice:
                return "Block Device";
            case FileType::Fifo:
                return "FIFO";
            case FileType::Unknown:
            default:
                return "Unknown";
        }
    }

    struct DirectoryEntry
    {
        using FileType = SharedData::FileType;

        std::filesystem::path path{};
        std::filesystem::path longName{};
        std::uint32_t flags{0};
        FileType type{FileType::Unknown};
        std::uint64_t size{0};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        std::string owner{};
        std::string group{};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        std::uint64_t atime{0};
        std::uint32_t atimeNsec{0};
        std::uint64_t createTime{0};
        std::uint32_t createTimeNsec{0};
        std::uint64_t mtime{0};
        std::uint32_t mtimeNsec{0};
        std::string acl{};

        bool isDirectory() const
        {
            return type == FileType::Directory;
        }
        bool isRegularFile() const
        {
            return type == FileType::Regular;
        }
        bool isSymlink() const
        {
            return type == FileType::Symlink;
        }
        bool isSpecial() const
        {
            return type == FileType::Special;
        }
        bool isUnknown() const
        {
            return type == FileType::Unknown;
        }
        bool isSocket() const
        {
            return type == FileType::Socket;
        }
        bool isCharDevice() const
        {
            return type == FileType::CharDevice;
        }
        bool isBlockDevice() const
        {
            return type == FileType::BlockDevice;
        }
        bool isFifo() const
        {
            return type == FileType::Fifo;
        }

        // Used for directory traversal. Avoids pointer instability in vector and unique_ptr
        std::optional<std::size_t> parent{std::nullopt};
    };

    inline std::filesystem::path fullPath(std::vector<DirectoryEntry> const& entries, DirectoryEntry const& entry)
    {
        if (entry.parent)
        {
            const auto parentIndex = entry.parent.value();
            if (parentIndex >= entries.size())
                throw std::out_of_range("Parent index is out of range");
            return fullPath(entries, entries[parentIndex]) / entry.path;
        }
        else
            return entry.path;
    }

    inline std::filesystem::path
    fullPathRelative(std::vector<DirectoryEntry> const& entries, DirectoryEntry const& entry)
    {
        if (entry.parent)
        {
            const auto parentIndex = entry.parent.value();
            if (parentIndex >= entries.size())
                throw std::out_of_range("Parent index is out of range");
            if (parentIndex == 0)
                return entry.path;
            return fullPathRelative(entries, entries[parentIndex]) / entry.path;
        }
        else
            return entry.path;
    }

    void to_json(nlohmann::json& j, DirectoryEntry const& entry);
    void from_json(nlohmann::json const& j, DirectoryEntry& entry);
}