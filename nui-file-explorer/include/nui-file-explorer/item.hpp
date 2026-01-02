#pragma once

#include <utility/describe.hpp>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <filesystem>
#include <string>
#include <cstdint>
#include <chrono>

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
        std::string owner{};
        std::string group{};
        std::uint64_t atime = 0;
        std::uint64_t size = 0;

        std::string lsStyleTypePermsUserGroup() const
        {
            using std::filesystem::perms;
            std::string cryptics = {
                [this]()
                {
                    switch (type)
                    {
                        case Type::Regular:
                            return '-';
                        case Type::Directory:
                            return 'd';
                        case Type::Symlink:
                            return 'l';
                        case Type::Socket:
                            return 's';
                        case Type::CharDevice:
                            return 'c';
                        case Type::BlockDevice:
                            return 'b';
                        case Type::Fifo:
                            return 'p';
                        default:
                            return '?';
                    }
                }(),
                perms::none == (permissions & perms::owner_read) ? '-' : 'r',
                perms::none == (permissions & perms::owner_write) ? '-' : 'w',
                perms::none == (permissions & perms::owner_exec) ? '-' : 'x',
                perms::none == (permissions & perms::group_read) ? '-' : 'r',
                perms::none == (permissions & perms::group_write) ? '-' : 'w',
                perms::none == (permissions & perms::group_exec) ? '-' : 'x',
                perms::none == (permissions & perms::others_read) ? '-' : 'r',
                perms::none == (permissions & perms::others_write) ? '-' : 'w',
                perms::none == (permissions & perms::others_exec) ? '-' : 'x',
            };

            return fmt::format("{} {} {}", cryptics, owner, group);
        }

        std::string readableATime() const
        {
            using namespace std::chrono;
            auto tp = system_clock::time_point{seconds{atime}};
            return fmt::format("{:%Y-%m-%d %H:%M:%S}", floor<seconds>(tp));
        }
    };

    BOOST_DESCRIBE_ENUM(
        Item::Type,
        Regular,
        Directory,
        Symlink,
        Special,
        Unknown,
        Socket,
        CharDevice,
        BlockDevice,
        Fifo
    )
}