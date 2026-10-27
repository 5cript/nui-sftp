#pragma once

#include <expected>
#include <filesystem>
#include <system_error>

namespace Utility
{
    /**
     * @brief Creates @p destination as a copy of the link @p source itself, pointing where @p source points.
     *
     * On Windows a junction becomes a junction to the same target, which unlike a symlink needs no privilege. Every
     * other link is copied by std::filesystem::copy_symlink. @p destination must not exist.
     */
    std::expected<void, std::error_code> copyLink(std::filesystem::path const& source, std::filesystem::path const& destination);
}
