#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>

namespace Utility
{
    namespace Detail
    {
        /**
         * @brief Reads the status of a path itself, like std::filesystem::symlink_status.
         */
        using SymlinkStatusReader =
            std::function<std::filesystem::file_status(std::filesystem::path const& path, std::error_code& error)>;

        /**
         * @brief checkedSymlinkStatus() with each look at @p path taken by @p readStatus.
         */
        std::expected<std::optional<std::filesystem::file_status>, std::error_code>
        checkedSymlinkStatus(std::filesystem::path const& path, SymlinkStatusReader const& readStatus);
    }

    /**
     * @brief The status of @p path itself, a symbolic link not followed, where a failed check never reads as "gone".
     *
     * Only "no such file or directory" (ENOENT, ENOTDIR, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND) means the path
     * does not exist. On Windows std::filesystem also reports a path past MAX_PATH and an unreachable network path as
     * not found, so that answer is checked again through the extended-length form (\\?\) of the path. A shorter path
     * found that way gets one more look, it was written anew meanwhile (git deletes and writes files). A path that is
     * being deleted (STATUS_DELETE_PENDING, until the last handle to it closes) or lies below one counts as gone.
     *
     * @return The status, nothing when the path does not exist, an error when that cannot be told: the path exists
     *         but cannot be reached as given (ERROR_FILENAME_EXCED_RANGE past MAX_PATH, ERROR_INVALID_NAME for a name
     *         only the extended-length form reaches), or the check failed.
     */
    std::expected<std::optional<std::filesystem::file_status>, std::error_code>
    checkedSymlinkStatus(std::filesystem::path const& path);
}
