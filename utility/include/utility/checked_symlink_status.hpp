#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <system_error>

namespace Utility
{
    /**
     * @brief The status of @p path itself, a symbolic link not followed, where a failed check never reads as "gone".
     *
     * Only "no such file or directory" (ENOENT, ENOTDIR, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND) means the path
     * does not exist. On Windows std::filesystem also reports a path past MAX_PATH and an unreachable network path as
     * not found, so that answer is checked again through the extended-length form (\\?\) of the path.
     *
     * @return The status, nothing when the path does not exist, an error when that cannot be told: the path exists
     *         but cannot be reached as given (ERROR_FILENAME_EXCED_RANGE past MAX_PATH), or the check failed.
     */
    std::expected<std::optional<std::filesystem::file_status>, std::error_code>
    checkedSymlinkStatus(std::filesystem::path const& path);
}
