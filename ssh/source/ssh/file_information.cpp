#include <ssh/file_information.hpp>
#include <ssh/u8_path.hpp>

namespace SecureShell
{
    FileInformation fromSftpAttributes(sftp_attributes attributes)
    {
        return FileInformation{
            SharedData::DirectoryEntry{
                // Server names are UTF-8, a narrow std::string would be read in the ANSI code page on Windows.
                .path = attributes->name ? pathFromU8(attributes->name) : std::filesystem::path{},
                .flags = attributes->flags,
                .type = static_cast<SharedData::FileType>(attributes->type),
                .size = attributes->size,
                .uid = attributes->uid,
                .gid = attributes->gid,
                .owner = attributes->owner ? std::string{attributes->owner} : std::string{},
                .group = attributes->group ? std::string{attributes->group} : std::string{},
                .permissions = static_cast<std::filesystem::perms>(attributes->permissions),
                .atime = attributes->atime,
                .atimeNsec = attributes->atime_nseconds,
                .createTime = attributes->createtime,
                .createTimeNsec = attributes->createtime_nseconds,
                .mtime = attributes->mtime,
                .mtimeNsec = attributes->mtime_nseconds,
                .acl = attributes->acl
                    ? std::string{ssh_string_get_char(attributes->acl), ssh_string_len(attributes->acl)}
                    : std::string{},
            },
        };
    }
}