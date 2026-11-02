#pragma once

#include <constants/sftp.hpp>
#include <remote_io/remote_file_system.hpp>

#include <cstdint>

/**
 * @brief Whether a transfer of @p size bytes takes the asynchronous path: a big file always, any file on a file system
 *        that prefers it.
 */
inline bool takesAsynchronousPath(RemoteIo::Capabilities const& capabilities, std::uint64_t size)
{
    return capabilities.preferAsyncTransfers || size >= Constants::bigFileCutOff;
}
