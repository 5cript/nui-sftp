#pragma once

#include <constants/sftp.hpp>

#include <cstdint>
#include <functional>

/**
 * @brief Whether a file is transferred in the unsafe fast mode, asked once when the file is opened. Unset or false is
 *        the safe mode.
 *
 * The mode saves the round trips of the safety mechanics: the file is written straight to its destination without a
 * temporary file, the destination is not checked before or after (existence, compared state), and a partial file is
 * not continued. A destination that may not be overwritten still fails the open, except on a file system that cannot
 * check what a write replaces (RemoteIo::Capabilities::probeEntries), where it is overwritten. A file that fails or is
 * stopped is removed where that is still possible, an overwritten one is lost.
 */
using UnsafeFastMode = std::function<bool()>;

/**
 * @brief How an upload goes to its destination.
 */
enum class UploadPath
{
    /** @brief Through a temporary file that replaces the destination once complete. */
    Safe,
    UnsafeFast,
    /** @brief Not at all: the file system cannot check the destination and the unsafe fast mode is off. */
    Refused,
};

/**
 * @brief Whether a file of @p size bytes is transferred in the unsafe fast mode that @p mode asks for, see
 *        Constants::unsafeFastSizeLimit.
 */
inline bool transfersUnsafeFast(UnsafeFastMode const& mode, std::uint64_t size)
{
    return size < Constants::unsafeFastSizeLimit && mode && mode();
}

/**
 * @brief How a file of @p size bytes is uploaded to a file system that @p checksDestinations or not, asking @p mode
 *        once. The safe mode needs the checks, so without them only the unsafe fast mode uploads, at any size.
 */
inline UploadPath uploadPathOf(UnsafeFastMode const& mode, std::uint64_t size, bool checksDestinations)
{
    if (checksDestinations)
        return transfersUnsafeFast(mode, size) ? UploadPath::UnsafeFast : UploadPath::Safe;
    return mode && mode() ? UploadPath::UnsafeFast : UploadPath::Refused;
}
