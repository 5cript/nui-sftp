#pragma once

#include <libssh/libssh.h>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace SecureShell
{
    /**
     * @brief Blocks until a socket can be read (or written) or another thread calls wake.
     *
     * The wake handle is a pipe on POSIX and a loopback UDP socket on Windows, where WSAPoll only takes sockets.
     */
    class SocketWait
    {
      public:
        /**
         * @brief What ended a wait.
         */
        enum class Result
        {
            Ready,
            Woken,
            TimedOut,
            Failed,
        };

        /**
         * @brief The socket to wait for and in which direction.
         */
        struct Watched
        {
            socket_t socket{SSH_INVALID_SOCKET};
            /** @brief Also wake when the socket can be written, while libssh has output it could not send yet. */
            bool writable{false};
        };

        static std::expected<std::unique_ptr<SocketWait>, std::string> create();

        ~SocketWait();
        SocketWait(SocketWait const&) = delete;
        SocketWait& operator=(SocketWait const&) = delete;
        SocketWait(SocketWait&&) = delete;
        SocketWait& operator=(SocketWait&&) = delete;

        /**
         * @brief Ends a running wait, or the next one if none runs. Callable from any thread.
         */
        void wake() noexcept;

        /**
         * @brief Waits for the socket, a wake or the deadline, whichever comes first.
         *
         * @param deadline No deadline waits until the socket or a wake.
         */
        Result wait(Watched const& watched, std::optional<std::chrono::steady_clock::time_point> deadline);

      private:
        struct Implementation;
        explicit SocketWait(std::unique_ptr<Implementation> impl);

      private:
        std::unique_ptr<Implementation> impl_;
    };
}
