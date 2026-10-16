#include <ssh/file_information.hpp>

#include <paths/utf8.hpp>

#include <string>
#include <string_view>

namespace SecureShell
{
    FileInformation fromSftpAttributes(sftp_attributes attributes)
    {
        const auto serverText = [](char const* text)
        {
            return text ? Paths::sanitizedUtf8(text) : std::string{};
        };
        return FileInformation{
            SharedData::DirectoryEntry{
                .name = serverText(attributes->name),
                .nameIsValidUtf8 = attributes->name == nullptr || Paths::isValidUtf8(attributes->name),
                .flags = attributes->flags,
                .type = static_cast<SharedData::FileType>(attributes->type),
                .size = attributes->size,
                .uid = attributes->uid,
                .gid = attributes->gid,
                .owner = serverText(attributes->owner),
                .group = serverText(attributes->group),
                .permissions = static_cast<std::filesystem::perms>(attributes->permissions),
                .atime = attributes->atime,
                .atimeNsec = attributes->atime_nseconds,
                .createTime = attributes->createtime,
                .createTimeNsec = attributes->createtime_nseconds,
                .mtime = attributes->mtime,
                .mtimeNsec = attributes->mtime_nseconds,
                .acl = attributes->acl
                    ? Paths::sanitizedUtf8({ssh_string_get_char(attributes->acl), ssh_string_len(attributes->acl)})
                    : std::string{},
            },
        };
    }
}