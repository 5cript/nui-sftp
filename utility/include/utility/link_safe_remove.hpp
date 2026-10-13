#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace Utility
{
    /**
     * @brief True for a symlink and, on Windows, for every reparse point such as a junction. std::filesystem may
     *        report a junction as a plain directory, and walking into it leaves the tree it belongs to.
     */
    bool isLinkOrJunction(std::filesystem::path const& path, std::error_code& error);

    /**
     * @brief Finds the first existing component of @p path below @p root (root excluded, @p path included) that is
     *        a symlink or junction. Writing through it would leave @p root.
     * @return The offending component, or nothing when there is none. @p error is set when @p path is not lexically
     *         below @p root or a component cannot be inspected.
     */
    std::optional<std::filesystem::path>
    firstLinkBelow(std::filesystem::path const& root, std::filesystem::path const& path, std::error_code& error);

    /**
     * @brief Removes @p path and, for a real directory, everything below it. A symlink or junction is removed
     *        itself and never followed, so nothing outside @p path is touched.
     * @return The number of removed entries, 0 when @p path does not exist.
     */
    std::uintmax_t removeAllNoFollow(std::filesystem::path const& path, std::error_code& error);
}
