#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>

namespace Utility
{
    /**
     * @brief Permissions that are safe to apply to a transferred file or directory.
     *
     * @param permissions Permissions of the source, unknown when the source did not report any.
     * @return nullopt for unknown permissions, otherwise the read, write and execute bits only. Setuid,
     *         setgid and sticky never travel with a transfer.
     */
    std::optional<std::filesystem::perms> sanitizePermissions(std::filesystem::perms permissions);

    /**
     * @brief The file mode creation mask of this process, empty where there is none.
     */
    std::filesystem::perms processUmask();

    /**
     * @brief @ref sanitizePermissions with the process umask applied, like the system does for a new file.
     */
    std::optional<std::filesystem::perms> inheritablePermissions(std::filesystem::perms permissions);

    /**
     * @brief The permissions the system gives a new file: read and write for everyone, minus the process umask.
     */
    std::filesystem::perms newFilePermissions();

    /**
     * @brief Creates @p path when it is missing and lets only its owner read and write it, so nothing written to it
     *        before its final permissions are set is readable by others. Never follows a link at @p path. Does
     *        nothing on Windows, where a new file takes the access rules of its directory.
     */
    std::expected<void, std::error_code> restrictToOwner(std::filesystem::path const& path);

    /**
     * @brief The permissions of a local file as far as they mean anything. Unknown on Windows, where they are made up
     *        from the read-only attribute and mark every file executable.
     */
    std::filesystem::perms localPermissions(std::filesystem::file_status const& status);
}
