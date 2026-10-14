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
     * MoveFileExW without MOVEFILE_REPLACE_EXISTING on Windows. Where the filesystem refuses those, a regular file
     * moves by link and unlink on Linux, and anything else by @ref Detail::renameUnlessExists, which leaves a window
     * between the check and the rename.
     */
    std::expected<void, std::error_code> renameNoReplace(std::filesystem::path const& from, std::filesystem::path const& to);

    /**
     * @brief Moves a finished file from @p from to @p to.
     *
     * @param replace Whether an existing @p to is replaced. Without it, an existing @p to fails with
     *        std::errc::file_exists, see @ref renameNoReplace.
     */
    std::expected<void, std::error_code>
    moveIntoPlace(std::filesystem::path const& from, std::filesystem::path const& to, bool replace);

    namespace Detail
    {
        /**
         * @brief Whether @p error of an atomic no-replace rename or a hard link means the filesystem does not offer
         *        it, rather than that the move itself failed.
         */
        bool isNoReplaceUnsupported(std::error_code error);

        /**
         * @brief Moves the regular file @p from to @p to by a hard link and removing @p from. The link fails on an
         *        existing @p to. When @p from cannot be removed, the link is removed again.
         */
        std::expected<void, std::error_code>
        renameByLink(std::filesystem::path const& from, std::filesystem::path const& to);

        /**
         * @brief Renames @p from to @p to unless something, a dangling link included, exists at @p to.
         */
        std::expected<void, std::error_code>
        renameUnlessExists(std::filesystem::path const& from, std::filesystem::path const& to);
    }
}
