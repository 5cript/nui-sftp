#pragma once

#include <paths/remote_path.hpp>
#include <utility/path_utf.hpp>

#include <expected>
#include <filesystem>

namespace Utility
{
    /**
     * @brief Validates a remote path that still travels as std::filesystem::path. Goes away once the callers
     *        carry Paths::RemotePath themselves.
     */
    inline std::expected<Paths::RemotePath, Paths::PathError> toRemotePath(std::filesystem::path const& path)
    {
        return Paths::RemotePath::make(pathToUtf8Generic(path));
    }

    /**
     * @brief A validated remote path, or an unset one that every SftpSession call refuses.
     */
    inline Paths::RemotePath toRemotePathOrUnset(std::filesystem::path const& path)
    {
        return toRemotePath(path).value_or(Paths::RemotePath{});
    }

    /**
     * @brief The std::filesystem::path form for code that has not moved to Paths::RemotePath yet.
     */
    inline std::filesystem::path toFilesystemPath(Paths::RemotePath const& path)
    {
        return pathFromUtf8(path.value());
    }
}
