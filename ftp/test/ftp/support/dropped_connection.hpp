#pragma once

#ifndef _WIN32

#    include <cstdint>
#    include <expected>
#    include <string>
#    include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief Drops every packet of one TCP connection in both directions inside a server container, with iptables,
     *        so neither end sees a FIN or a reset. The container needs NET_ADMIN, which the throttled variants have.
     *        With the object the server's end of the connection is killed and the rules go again. Each rule names
     *        the process that added it, drop() first deletes those of processes that ended without taking theirs back.
     */
    class DroppedConnection
    {
      public:
        /**
         * @param serverPort The server's end of the connection.
         * @param clientPort The client's end of the connection.
         * @return The connection, dropped from now on, or what iptables said.
         */
        static std::expected<DroppedConnection, std::string>
        drop(std::string container, std::uint16_t serverPort, std::uint16_t clientPort);

        /**
         * @brief Kills the server's end of the connection and deletes exactly the rules drop() added.
         */
        ~DroppedConnection();
        DroppedConnection(DroppedConnection const&) = delete;
        DroppedConnection& operator=(DroppedConnection const&) = delete;
        DroppedConnection(DroppedConnection&& other) noexcept;
        DroppedConnection& operator=(DroppedConnection&& other) noexcept;

      private:
        DroppedConnection(
            std::string container,
            std::uint16_t serverPort,
            std::uint16_t clientPort,
            std::vector<std::string> rules
        );
        void restore();

        std::string container_{};
        std::uint16_t serverPort_{0u};
        std::uint16_t clientPort_{0u};
        /** @brief Chain and match of each rule, as -I took them and -D takes them back. */
        std::vector<std::string> rules_{};
    };
}

#endif
