#pragma once

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
 * @brief Files of this size and above keep the safe mode: the saved round trips are little against their transfer,
 *        and an interrupted one can be continued.
 */
constexpr std::uint64_t unsafeFastSizeLimit = 1024u * 1024u;

/**
 * @brief Whether a file of @p size bytes is transferred in the unsafe fast mode that @p mode asks for.
 */
inline bool transfersUnsafeFast(UnsafeFastMode const& mode, std::uint64_t size)
{
    return size < unsafeFastSizeLimit && mode && mode();
}
