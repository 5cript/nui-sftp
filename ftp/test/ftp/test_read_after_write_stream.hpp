#pragma once

#include <ftp/client/read_after_write_stream.hpp>
#include <ftp/support/test_pki.hpp>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;

        struct HeldWrites
        {
            bool holding{false};
            std::size_t readsCompleted{0u};
            std::optional<asio::any_completion_handler<void()>> held{};
        };

        /**
         * @brief A socket whose write completions wait while the test holds them, the bytes are out already: a write
         *        through on the wire whose handler has not run yet. Counts the reads it completed.
         */
        class HeldWriteSocket
        {
          public:
            using executor_type = asio::ip::tcp::socket::executor_type;
            using lowest_layer_type = asio::ip::tcp::socket::lowest_layer_type;

            HeldWriteSocket(asio::ip::tcp::socket& socket, std::shared_ptr<HeldWrites> writes)
                : socket_{&socket}
                , writes_{std::move(writes)}
            {}

            executor_type get_executor()
            {
                return socket_->get_executor();
            }

            lowest_layer_type& lowest_layer()
            {
                return socket_->lowest_layer();
            }

            lowest_layer_type const& lowest_layer() const
            {
                return socket_->lowest_layer();
            }

            template <typename BuffersT, typename HandlerT>
            void async_read_some(BuffersT const& buffers, HandlerT&& handler)
            {
                socket_->async_read_some(
                    buffers,
                    [writes = writes_, handler = std::forward<HandlerT>(handler)](
                        boost::system::error_code error, std::size_t size
                    ) mutable
                    {
                        ++writes->readsCompleted;
                        std::move(handler)(error, size);
                    }
                );
            }

            template <typename BuffersT, typename HandlerT>
            void async_write_some(BuffersT const& buffers, HandlerT&& handler)
            {
                socket_->async_write_some(
                    buffers,
                    [writes = writes_, handler = std::forward<HandlerT>(handler)](
                        boost::system::error_code error, std::size_t size
                    ) mutable
                    {
                        if (!writes->holding)
                        {
                            std::move(handler)(error, size);
                            return;
                        }
                        writes->held.emplace(
                            [handler = std::move(handler), error, size]() mutable
                            {
                                std::move(handler)(error, size);
                            }
                        );
                    }
                );
            }

          private:
            asio::ip::tcp::socket* socket_;
            std::shared_ptr<HeldWrites> writes_;
        };

        /**
         * @brief Runs @p context until @p done, bounded like the waits of the connection tests. The context stops each
         *        time it runs out of work in between.
         */
        bool runUntil(asio::io_context& context, std::function<bool()> const& done)
        {
            context.restart();
            bool ran = true;
            while (ran && !done())
                ran = context.run_one_for(std::chrono::seconds{20}) > 0u;
            return done();
        }
    }

    TEST(ReadAfterWriteStreamTests, RefusalOfARenegotiationDuringAWriteLeavesWithoutAnotherWrite)
    {
        const auto pki = testPki();
        ASSERT_TRUE(pki) << pki.error();
        asio::io_context context{};
        asio::ip::tcp::acceptor acceptor{context, asio::ip::tcp::endpoint{asio::ip::address_v4::loopback(), 0u}};
        asio::ip::tcp::socket clientSocket{context};
        clientSocket.connect(acceptor.local_endpoint());
        asio::ip::tcp::socket serverSocket = acceptor.accept();

        asio::ssl::context serverContext{asio::ssl::context::tls_server};
        serverContext.use_certificate_chain(asio::buffer(pki->get().server.certificate));
        serverContext.use_private_key(asio::buffer(pki->get().server.privateKey), asio::ssl::context::pem);
        SSL_CTX_set_max_proto_version(serverContext.native_handle(), TLS1_2_VERSION);
        asio::ssl::context clientContext{asio::ssl::context::tls_client};
        // As the client's TlsContext does.
        SSL_CTX_set_options(clientContext.native_handle(), SSL_OP_NO_RENEGOTIATION);

        asio::ssl::stream<asio::ip::tcp::socket&> server{serverSocket, serverContext};
        const auto writes = std::make_shared<HeldWrites>();
        asio::ssl::stream<Detail::ReadAfterWriteStream<HeldWriteSocket>> client{
            HeldWriteSocket{clientSocket, writes}, clientContext
        };

        std::optional<boost::system::error_code> serverShook{};
        std::optional<boost::system::error_code> clientShook{};
        server.async_handshake(
            asio::ssl::stream_base::server,
            [&serverShook](boost::system::error_code error)
            {
                serverShook = error;
            }
        );
        client.async_handshake(
            asio::ssl::stream_base::client,
            [&clientShook](boost::system::error_code error)
            {
                clientShook = error;
            }
        );
        ASSERT_TRUE(runUntil(
            context,
            [&serverShook, &clientShook]()
            {
                return serverShook && clientShook;
            }
        ));
        ASSERT_FALSE(*serverShook) << serverShook->message();
        ASSERT_FALSE(*clientShook) << clientShook->message();

        // A read always waits on the control connection.
        std::array<char, 64> clientBuffer{};
        std::optional<boost::system::error_code> clientRead{};
        client.async_read_some(
            asio::buffer(clientBuffer),
            [&clientRead](boost::system::error_code error, std::size_t)
            {
                clientRead = error;
            }
        );

        writes->holding = true;
        const std::string command{"NOOP\r\n"};
        std::optional<boost::system::error_code> written{};
        asio::async_write(
            client,
            asio::buffer(command),
            [&written](boost::system::error_code error, std::size_t)
            {
                written = error;
            }
        );
        std::array<char, 64> serverBuffer{};
        std::optional<std::size_t> serverReceived{};
        server.async_read_some(
            asio::buffer(serverBuffer),
            [&serverReceived](boost::system::error_code, std::size_t size)
            {
                serverReceived = size;
            }
        );
        ASSERT_TRUE(runUntil(
            context,
            [&serverReceived]()
            {
                return serverReceived.has_value();
            }
        ));
        ASSERT_EQ(*serverReceived, command.size());

        // The HelloRequest comes in while the handler of the write has not run yet.
        ASSERT_EQ(SSL_renegotiate(server.native_handle()), 1);
        std::optional<boost::system::error_code> requested{};
        server.async_handshake(
            asio::ssl::stream_base::server,
            [&requested](boost::system::error_code error)
            {
                requested = error;
            }
        );
        const auto readsBefore = writes->readsCompleted;
        ASSERT_TRUE(runUntil(
            context,
            [&requested, &writes, readsBefore]()
            {
                return requested && writes->readsCompleted > readsBefore;
            }
        ));
        ASSERT_FALSE(*requested) << requested->message();
        ASSERT_TRUE(writes->held);

        std::optional<boost::system::error_code> refused{};
        server.async_read_some(
            asio::buffer(serverBuffer),
            [&refused](boost::system::error_code error, std::size_t)
            {
                refused = error;
            }
        );
        asio::post(
            context,
            [writes]()
            {
                writes->holding = false;
                auto held = std::move(*writes->held);
                writes->held.reset();
                std::move(held)();
            }
        );
        // Nothing is written behind the command, the refusal leaves on its own.
        ASSERT_TRUE(runUntil(
            context,
            [&refused, &written]()
            {
                return refused && written;
            }
        ));
        EXPECT_FALSE(*written) << written->message();
        EXPECT_EQ(ERR_GET_REASON(static_cast<unsigned long>(refused->value())), SSL_R_NO_RENEGOTIATION)
            << refused->message();
    }
}
