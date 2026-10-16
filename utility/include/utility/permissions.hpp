#pragma once

#include <filesystem>
#include <optional>

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
     * @brief The permissions of a local file as far as they mean anything. Unknown on Windows, where they are made up
     *        from the read-only attribute and mark every file executable.
     */
    std::filesystem::perms localPermissions(std::filesystem::file_status const& status);
}
