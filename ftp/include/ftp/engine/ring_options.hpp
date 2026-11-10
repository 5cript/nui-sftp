#pragma once

#include <cstddef>

namespace FileTransferProtocol
{
    /**
     * @brief The slots of the ring of one asynchronous transfer.
     */
    struct RingOptions
    {
        /** @brief How many chunks one side runs ahead of the other at most. */
        std::size_t slots{4u};
        /** @brief The bytes of one slot, every chunk but the last fills one. */
        std::size_t chunkSize{256u * 1024u};
    };
}
