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
 * not continued. A destination that may not be overwritten still fails the open. A file that fails or is stopped is
 * removed where that is still possible, an overwritten one is lost.
 */
using UnsafeFastMode = std::function<bool()>;

/**
 * @brief Whether a file of @p size bytes is transferred in the unsafe fast mode that @p mode asks for, see
 *        Constants::unsafeFastSizeLimit.
 */
inline bool transfersUnsafeFast(UnsafeFastMode const& mode, std::uint64_t size)
{
    return size < Constants::unsafeFastSizeLimit && mode && mode();
}
