#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/login.hpp>
#include <ftp/client/transfer.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace FileTransferProtocol
{
    /**
     * @brief A logged-in control connection of a Pool. The connection belongs to its own strand, everything else to
     *        the session strand of the pool. The login and the transfer session are set before the connection is
     *        handed out and never change.
     */
    struct PooledConnection
    {
        PooledConnection(
            std::string connectionName,
            std::shared_ptr<ControlConnection> controlConnection,
            boost::asio::any_io_executor sessionExecutor
        )
            : name{std::move(connectionName)}
            , control{std::move(controlConnection)}
            , idleTimer{std::move(sessionExecutor)}
        {}

        /** @brief C0 for the metadata connection, C1 and up for transfer connections, for the logs. */
        const std::string name;
        const std::shared_ptr<ControlConnection> control;
        LoginResult login{};
        TransferSession transferSession{};
        /** @brief Ends the idle time: the linger of a transfer connection, the keepalive wait of C0. */
        boost::asio::steady_timer idleTimer;
        /** @brief Tells a wait of idleTimer that ran out from one armed after it. */
        std::uint64_t idleGeneration{0u};
        bool leased{false};
        /** @brief Leased only for the check that it is still open, an end now is the end of an idle connection. */
        bool checking{false};
        /** @brief Out of the pool for good: lost, closed, or ended with QUIT after its linger. */
        bool dropped{false};
    };
}
