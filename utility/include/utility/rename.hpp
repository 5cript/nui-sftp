#pragma once

#include <expected>
#include <filesystem>
#include <system_error>

namespace Utility
{
    /**
     * @brief Renames @p from to @p to and fails with std::errc::file_exists instead of replacing an existing @p to.
     *
     * Atomic where the platform offers it: renameat2 RENAME_NOREPLACE on Linux, renamex_np RENAME_EXCL on macOS,
     * MoveFileExW without MOVEFILE_REPLACE_EXISTING on Windows. A Linux filesystem without RENAME_NOREPLACE falls
     * back to link and unlink for files.
     */
    std::expected<void, std::error_code> renameNoReplace(std::filesystem::path const& from, std::filesystem::path const& to);
}
