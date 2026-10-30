#pragma once

#include <cstdint>

namespace RemoteIo
{
#ifdef _WIN32
    /**
     * @brief A socket handle of the operating system: SOCKET on Windows.
     */
    using NativeSocket = std::uintptr_t;

    /**
     * @brief The handle of no socket: INVALID_SOCKET on Windows.
     */
    inline constexpr NativeSocket invalidNativeSocket = ~NativeSocket{0};
#else
    /**
     * @brief A socket handle of the operating system: a file descriptor.
     */
    using NativeSocket = int;

    /**
     * @brief The handle of no socket.
     */
    inline constexpr NativeSocket invalidNativeSocket = -1;
#endif
}
