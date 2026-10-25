#pragma once

#include <cstdint>

namespace Constants
{
    constexpr static int bigFileCutOff = 16384; // 1 buffer size
    /**
     * @brief Files of this size and above keep the safe mode in the unsafe fast mode: the saved round trips are little
     *        against their transfer, and an interrupted one can be continued.
     */
    constexpr static std::uint64_t unsafeFastSizeLimit = 1024u * 1024u;
}
