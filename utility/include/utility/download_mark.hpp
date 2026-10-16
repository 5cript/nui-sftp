#pragma once

#include <filesystem>
#include <system_error>

namespace Utility
{
    /**
     * @brief Marks @p path as downloaded from the internet, so the system warns before running it: the Mark of the
     *        Web (Zone.Identifier stream, internet zone) on Windows, the quarantine attribute on macOS. Nothing
     *        elsewhere. A filesystem without streams or extended attributes reports an error, the file is unchanged.
     */
    std::error_code markAsDownloaded(std::filesystem::path const& path);
}
