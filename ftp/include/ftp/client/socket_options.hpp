#pragma once

#include <remote_io/remote_error.hpp>

#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <expected>
#include <optional>

namespace FileTransferProtocol
{
    /**
     * @brief TCP keepalive of a control socket. It probes an idle connection, so a dead peer or a NAT that forgot the
     *        mapping is noticed after idle + interval × count.
     */
    struct KeepaliveOptions
    {
        bool enabled{true};
        std::chrono::seconds idle{5};
        std::chrono::seconds interval{2};
        int count{3};
    };

    struct ControlSocketOptions
    {
        KeepaliveOptions keepalive{};
        /**
         * @brief Linux only: sent data the peer does not acknowledge ends the connection after this time, keepalive
         *        does not probe while such data is in flight. Nothing takes idle + interval × count of the keepalive.
         */
        std::optional<std::chrono::milliseconds> userTimeout{};
    };

    /**
     * @brief The TCP_USER_TIMEOUT that @p options set on Linux.
     */
    std::chrono::milliseconds effectiveUserTimeout(ControlSocketOptions const& options);

    /**
     * @brief Sets TCP_NODELAY, the keepalive and on Linux TCP_USER_TIMEOUT on a connected socket. Where a system has no
     *        option for the keepalive idle time, interval or count, it keeps its own value.
     *
     * @return A Failure naming the first option the system refused.
     */
    std::expected<void, RemoteIo::RemoteError>
    applyControlSocketOptions(boost::asio::ip::tcp::socket& socket, ControlSocketOptions const& options);
}
