#pragma once

#include <ftp/client/socket_options.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <gtest/gtest.h>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#endif

#include <chrono>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using boost::asio::ip::tcp;
        using namespace std::chrono_literals;

        /**
         * @brief Both ends of a loopback connection.
         */
        struct LoopbackConnection
        {
            boost::asio::io_context context{};
            tcp::acceptor acceptor{context, tcp::endpoint{boost::asio::ip::address_v4::loopback(), 0}};
            tcp::socket client{context};
            tcp::socket server{context};

            LoopbackConnection()
            {
                client.connect(acceptor.local_endpoint());
                server = acceptor.accept();
            }
        };

        template <int Level, int Name>
        int integerOptionOf(tcp::socket& socket)
        {
            boost::asio::detail::socket_option::integer<Level, Name> option{};
            socket.get_option(option);
            return option.value();
        }

        void expectKeepaliveTimes(
            [[maybe_unused]] tcp::socket& socket,
            [[maybe_unused]] int idle,
            [[maybe_unused]] int interval,
            [[maybe_unused]] int count
        )
        {
#if defined(TCP_KEEPIDLE)
            EXPECT_EQ((integerOptionOf<IPPROTO_TCP, TCP_KEEPIDLE>(socket)), idle);
#elif defined(TCP_KEEPALIVE)
            EXPECT_EQ((integerOptionOf<IPPROTO_TCP, TCP_KEEPALIVE>(socket)), idle);
#endif
#if defined(TCP_KEEPINTVL)
            EXPECT_EQ((integerOptionOf<IPPROTO_TCP, TCP_KEEPINTVL>(socket)), interval);
#endif
#if defined(TCP_KEEPCNT)
            EXPECT_EQ((integerOptionOf<IPPROTO_TCP, TCP_KEEPCNT>(socket)), count);
#endif
        }

        void expectUserTimeout([[maybe_unused]] tcp::socket& socket, [[maybe_unused]] std::chrono::milliseconds timeout)
        {
#if defined(TCP_USER_TIMEOUT)
            EXPECT_EQ((integerOptionOf<IPPROTO_TCP, TCP_USER_TIMEOUT>(socket)), timeout.count());
#endif
        }
    }

    TEST(ControlSocketOptionsTests, NoDelayKeepaliveAndUserTimeout)
    {
        LoopbackConnection connection{};
        const ControlSocketOptions options{.keepalive = {.idle = 7s, .interval = 3s, .count = 4}};
        const auto applied = applyControlSocketOptions(connection.client, options);
        ASSERT_TRUE(applied) << applied.error().toString();

        tcp::no_delay noDelay{};
        connection.client.get_option(noDelay);
        EXPECT_TRUE(noDelay.value());
        boost::asio::socket_base::keep_alive keepAlive{};
        connection.client.get_option(keepAlive);
        EXPECT_TRUE(keepAlive.value());
        expectKeepaliveTimes(connection.client, 7, 3, 4);
        // Keepalive gives up after idle + interval × count, so does unacknowledged data.
        EXPECT_EQ(effectiveUserTimeout(options), 19s);
        expectUserTimeout(connection.client, 19s);
    }

    TEST(ControlSocketOptionsTests, ChosenUserTimeout)
    {
        LoopbackConnection connection{};
        const ControlSocketOptions options{.userTimeout = 2500ms};
        ASSERT_TRUE(applyControlSocketOptions(connection.client, options));
        EXPECT_EQ(effectiveUserTimeout(options), 2500ms);
        expectUserTimeout(connection.client, 2500ms);
    }

    TEST(ControlSocketOptionsTests, KeepaliveOff)
    {
        LoopbackConnection connection{};
        ASSERT_TRUE(
            applyControlSocketOptions(connection.client, ControlSocketOptions{.keepalive = {.enabled = false}})
        );
        boost::asio::socket_base::keep_alive keepAlive{};
        connection.client.get_option(keepAlive);
        EXPECT_FALSE(keepAlive.value());
    }

    TEST(ControlSocketOptionsTests, ClosedSocketIsRefused)
    {
        boost::asio::io_context context{};
        tcp::socket socket{context};
        const auto applied = applyControlSocketOptions(socket, ControlSocketOptions{});
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().kind, RemoteIo::RemoteErrorKind::Failure);
        EXPECT_EQ(applied.error().protocol, RemoteIo::Protocol::Ftp);
    }

    TEST(ControlSocketOptionsTests, TuningOptionTheSystemDoesNotKnowKeepsItsValue)
    {
        LoopbackConnection connection{};
        // No system has a TCP option with this number, like an older one lacks TCP_KEEPIDLE its headers name.
        constexpr int unknownOption = 0x7fff;
        const auto applied =
            Detail::setTuningOption(connection.client, IPPROTO_TCP, unknownOption, 1, "an option no system has");
        EXPECT_TRUE(applied) << applied.error().toString();
    }

    TEST(ControlSocketOptionsTests, TuningOptionOnAClosedSocketIsRefused)
    {
        boost::asio::io_context context{};
        tcp::socket socket{context};
        const auto applied = Detail::setTuningOption(socket, IPPROTO_TCP, TCP_NODELAY, 1, "TCP_NODELAY");
        ASSERT_FALSE(applied);
        EXPECT_EQ(applied.error().kind, RemoteIo::RemoteErrorKind::Failure);
    }
}
