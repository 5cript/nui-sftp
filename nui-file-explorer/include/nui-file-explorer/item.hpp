#pragma once

#include <filesystem>
#include <string>
#include <cstdint>

namespace NuiFileExplorer
{
    struct Item
    {
        enum class Type
        {
            Regular = 1,
            Directory = 2,
            Symlink = 3,
            Special = 4,
            Unknown = 5,
            Socket = 6,
            CharDevice = 7,
            BlockDevice = 8,
            Fifo = 9
        };

        std::filesystem::path path;
        std::string icon; // url or base64 url etc.
        Type type = Type::Unknown;
        std::filesystem::perms permissions = std::filesystem::perms::none;
        unsigned int ownerId = 0;
        unsigned int groupId = 0;
        std::uint64_t atime = 0;
        std::uint64_t size = 0;
    };
}