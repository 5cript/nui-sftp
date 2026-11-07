#include <ftp/support/test_ftp_client.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <fmt/format.h>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <charconv>
#include <optional>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;
        namespace ssl = boost::asio::ssl;
        using boost::system::error_code;
        using tcp = asio::ip::tcp;

        template <auto release>
        struct Release
        {
            template <typename Resource>
            void operator()(Resource* resource) const noexcept
            {
                release(resource);
            }
        };
        using Session = std::unique_ptr<SSL_SESSION, Release<SSL_SESSION_free>>;
        using Certificate = std::unique_ptr<X509, Release<X509_free>>;
        using MemoryBio = std::unique_ptr<BIO, Release<BIO_free_all>>;

        /**
         * @brief A socket that may have turned to TLS.
         */
        struct Connection
        {
            explicit Connection(asio::io_context& ioContext)
                : socket{ioContext}
            {}

            tcp::socket socket;
            std::optional<ssl::stream<tcp::socket&>> tls{};
        };
    }

    class TestFtpClient::Implementation
    {
      public:
        explicit Implementation(std::chrono::milliseconds timeout)
            : timeout_{timeout}
        {}

        std::expected<void, std::string> connect(std::string const& host, std::uint16_t port)
        {
            control_.emplace(ioContext_);
            return connectSocket(control_->socket, host, port);
        }

        std::expected<void, std::string> startTls(TlsVerification const& verification)
        {
            if (!control_)
                return std::unexpected("Not connected");
            if (!received_.empty())
                return std::unexpected("Plaintext follows the reply that started TLS");
            try
            {
                context_.emplace(ssl::context::tls_client);
                if (verification.authority)
                {
                    context_->add_certificate_authority(asio::buffer(*verification.authority));
                    context_->set_verify_mode(ssl::verify_peer);
                }
                control_->tls.emplace(control_->socket, *context_);
                if (verification.authority)
                {
                    control_->tls->set_verify_callback(ssl::host_name_verification{verification.hostName});
                }
            }
            catch (boost::system::system_error const& error)
            {
                return std::unexpected(fmt::format("Preparing TLS: {}", error.what()));
            }
            return handshake(*control_->tls);
        }

        std::expected<void, std::string> send(std::string_view line)
        {
            if (!control_)
                return std::unexpected("Not connected");
            const auto text = fmt::format("{}\r\n", line);
            return write(*control_, text);
        }

        std::expected<Reply, std::string> readReply()
        {
            if (!control_)
                return std::unexpected("Not connected");

            Reply reply{};
            bool complete = false;
            while (!complete)
            {
                const auto line = readLine();
                if (!line)
                    return std::unexpected(line.error());
                if (!reply.text.empty())
                    reply.text += '\n';
                reply.text += *line;
                if (reply.code == 0)
                {
                    const auto parsed = std::from_chars(
                        line->data(), line->data() + std::min<std::size_t>(line->size(), 3u), reply.code
                    );
                    if (parsed.ec != std::errc{} || line->size() < 3u)
                        return std::unexpected(fmt::format("Not a reply: '{}'", *line));
                    complete = line->size() == 3u || (*line)[3] != '-';
                }
                else
                    complete = line->size() >= 4u && line->starts_with(fmt::format("{} ", reply.code));
            }
            return reply;
        }

        std::expected<void, std::string> openData(std::string const& host, std::uint16_t port, bool secured)
        {
            data_.emplace(ioContext_);
            const auto connected = connectSocket(data_->socket, host, port);
            if (!connected || !secured)
                return connected;
            if (!control_ || !control_->tls)
                return std::unexpected("The control connection has no TLS");

            data_->tls.emplace(data_->socket, *context_);
            // One session per data connection, OpenSSL marks a shared one bad when a connection ends without notify.
            const Session session{SSL_get1_session(control_->tls->native_handle())};
            if (session)
            {
                const Session copy{SSL_SESSION_dup(session.get())};
                if (copy)
                    SSL_set_session(data_->tls->native_handle(), copy.get());
            }
            return handshake(*data_->tls);
        }

        std::expected<std::string, std::string> readData()
        {
            if (!data_)
                return std::unexpected("No data connection");
            std::string received{};
            std::array<char, 16u * 1024u> chunk{};
            error_code error{};
            while (!error)
            {
                const auto [readError, size] = complete(
                    [&](auto handler)
                    {
                        if (data_->tls)
                            data_->tls->async_read_some(asio::buffer(chunk), handler);
                        else
                            data_->socket.async_read_some(asio::buffer(chunk), handler);
                    }
                );
                received.append(chunk.data(), size);
                error = readError;
            }
            if (error != asio::error::eof)
                return std::unexpected(fmt::format("Reading data: {}", error.message()));
            return received;
        }

        std::expected<void, std::string> writeData(std::string_view bytes)
        {
            if (!data_)
                return std::unexpected("No data connection");
            return write(*data_, bytes);
        }

        void closeData()
        {
            if (!data_)
                return;
            if (data_->tls)
            {
                complete(
                    [&](auto handler)
                    {
                        data_->tls->async_shutdown(
                            [handler](error_code error) mutable
                            {
                                handler(error, 0u);
                            }
                        );
                    }
                );
            }
            data_.reset();
        }

        std::optional<std::string> dataCertificate()
        {
            if (!data_ || !data_->tls)
                return std::nullopt;
            const Certificate certificate{SSL_get1_peer_certificate(data_->tls->native_handle())};
            const MemoryBio memory{BIO_new(BIO_s_mem())};
            if (!certificate || !memory || PEM_write_bio_X509(memory.get(), certificate.get()) != 1)
                return std::nullopt;
            char* text = nullptr;
            const auto size = BIO_get_mem_data(memory.get(), &text);
            return std::string(text, static_cast<std::size_t>(size));
        }

        void close()
        {
            data_.reset();
            control_.reset();
        }

      private:
        /**
         * @brief Runs one asynchronous operation with the timeout of this client.
         */
        template <typename Start>
        std::pair<error_code, std::size_t> complete(Start start)
        {
            std::optional<std::pair<error_code, std::size_t>> outcome{};
            ioContext_.restart();
            start(
                [&outcome](error_code error, std::size_t size)
                {
                    outcome.emplace(error, size);
                }
            );
            ioContext_.run_for(timeout_);
            if (outcome)
                return *outcome;

            error_code ignored;
            if (control_)
                control_->socket.cancel(ignored);
            if (data_)
                data_->socket.cancel(ignored);
            ioContext_.restart();
            ioContext_.run();
            return {asio::error::timed_out, 0u};
        }

        std::expected<void, std::string> connectSocket(tcp::socket& socket, std::string const& host, std::uint16_t port)
        {
            error_code error;
            const auto address = asio::ip::make_address(host, error);
            if (error)
                return std::unexpected(fmt::format("Not an address: {}", host));
            const auto [connectError, size] = complete(
                [&](auto handler)
                {
                    socket.async_connect(
                        tcp::endpoint{address, port},
                        [handler](error_code connected) mutable
                        {
                            handler(connected, 0u);
                        }
                    );
                }
            );
            if (connectError)
                return std::unexpected(fmt::format("Connecting to {}: {}", host, connectError.message()));
            return {};
        }

        std::expected<void, std::string> handshake(ssl::stream<tcp::socket&>& stream)
        {
            const auto [error, size] = complete(
                [&](auto handler)
                {
                    stream.async_handshake(
                        ssl::stream_base::client,
                        [handler](error_code handshaken) mutable
                        {
                            handler(handshaken, 0u);
                        }
                    );
                }
            );
            if (error)
                return std::unexpected(fmt::format("TLS handshake: {}", error.message()));
            return {};
        }

        std::expected<void, std::string> write(Connection& connection, std::string_view bytes)
        {
            const auto [error, size] = complete(
                [&](auto handler)
                {
                    if (connection.tls)
                        asio::async_write(*connection.tls, asio::buffer(bytes), handler);
                    else
                        asio::async_write(connection.socket, asio::buffer(bytes), handler);
                }
            );
            if (error)
                return std::unexpected(fmt::format("Writing: {}", error.message()));
            return {};
        }

        std::expected<std::string, std::string> readLine()
        {
            auto end = received_.find("\r\n");
            while (end == std::string::npos)
            {
                std::array<char, 4096u> chunk{};
                const auto [error, size] = complete(
                    [&](auto handler)
                    {
                        if (control_->tls)
                            control_->tls->async_read_some(asio::buffer(chunk), handler);
                        else
                            control_->socket.async_read_some(asio::buffer(chunk), handler);
                    }
                );
                received_.append(chunk.data(), size);
                if (error)
                    return std::unexpected(fmt::format("Reading a reply: {}", error.message()));
                end = received_.find("\r\n");
            }
            auto line = received_.substr(0u, end);
            received_.erase(0u, end + 2u);
            return line;
        }

      private:
        const std::chrono::milliseconds timeout_;
        asio::io_context ioContext_{1};
        std::optional<ssl::context> context_{};
        std::optional<Connection> control_{};
        std::optional<Connection> data_{};
        std::string received_{};
    };

    TestFtpClient::TestFtpClient(std::chrono::milliseconds timeout)
        : impl_{std::make_unique<Implementation>(timeout)}
    {}
    TestFtpClient::~TestFtpClient() = default;

    std::expected<void, std::string> TestFtpClient::connect(std::string const& host, std::uint16_t port)
    {
        return impl_->connect(host, port);
    }
    std::expected<void, std::string> TestFtpClient::startTls(TlsVerification const& verification)
    {
        return impl_->startTls(verification);
    }
    std::expected<void, std::string> TestFtpClient::send(std::string_view line)
    {
        return impl_->send(line);
    }
    std::expected<TestFtpClient::Reply, std::string> TestFtpClient::readReply()
    {
        return impl_->readReply();
    }
    std::expected<TestFtpClient::Reply, std::string> TestFtpClient::command(std::string_view line)
    {
        return send(line).and_then(
            [this]()
            {
                return readReply();
            }
        );
    }
    std::expected<void, std::string> TestFtpClient::openData(std::string const& host, std::uint16_t port, bool secured)
    {
        return impl_->openData(host, port, secured);
    }
    std::expected<std::string, std::string> TestFtpClient::readData()
    {
        return impl_->readData();
    }
    std::expected<void, std::string> TestFtpClient::writeData(std::string_view bytes)
    {
        return impl_->writeData(bytes);
    }
    void TestFtpClient::closeData()
    {
        impl_->closeData();
    }
    std::optional<std::string> TestFtpClient::dataCertificate()
    {
        return impl_->dataCertificate();
    }
    void TestFtpClient::close()
    {
        impl_->close();
    }

    std::optional<std::uint16_t> TestFtpClient::passivePort(Reply const& reply)
    {
        std::uint32_t port = 0u;
        if (reply.code == 229)
        {
            const auto start = reply.text.find("(|||");
            if (start == std::string::npos)
                return std::nullopt;
            auto const* digits = reply.text.data() + start + 4u;
            if (std::from_chars(digits, reply.text.data() + reply.text.size(), port).ec != std::errc{})
                return std::nullopt;
            return static_cast<std::uint16_t>(port);
        }

        const auto start = reply.text.find('(');
        if (reply.code != 227 || start == std::string::npos)
            return std::nullopt;
        std::array<std::uint32_t, 6u> numbers{};
        auto const* position = reply.text.data() + start + 1u;
        auto const* const end = reply.text.data() + reply.text.size();
        for (auto& number : numbers)
        {
            const auto parsed = std::from_chars(position, end, number);
            if (parsed.ec != std::errc{})
                return std::nullopt;
            position = parsed.ptr + 1;
        }
        return static_cast<std::uint16_t>(numbers[4] * 256u + numbers[5]);
    }
}
