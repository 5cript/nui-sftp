#pragma once

#include <shared_data/directory_entry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace TarArchive
{
    /**
     * @brief What a tar record says about one member.
     */
    struct MemberHeader
    {
        /**
         * @brief The member path, '/' separated and relative. Read from an archive it is untrusted text that can
         *        hold "..", an absolute path or anything else.
         */
        std::string path{};
        SharedData::FileType type{SharedData::FileType::Regular};
        std::uint64_t size{0};
        std::uint32_t uid{0};
        std::uint32_t gid{0};
        std::string owner{};
        std::string group{};
        std::filesystem::perms permissions{std::filesystem::perms::unknown};
        std::uint64_t mtime{0};
        /** @brief For symlinks: the link literal as UTF-8. */
        std::optional<std::string> linkTarget{};
    };

    /**
     * @brief The header for @p entry stored under @p path.
     */
    inline MemberHeader memberHeaderOf(std::string path, SharedData::DirectoryEntry const& entry)
    {
        return MemberHeader{
            .path = std::move(path),
            .type = entry.type,
            .size = entry.size,
            .uid = entry.uid,
            .gid = entry.gid,
            .owner = entry.owner,
            .group = entry.group,
            .permissions = entry.permissions,
            .mtime = entry.mtime,
            .linkTarget = entry.linkTarget,
        };
    }
}
