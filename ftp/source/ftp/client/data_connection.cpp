#include <ftp/client/data_connection.hpp>
#include <ftp/protocol/reply_parsers.hpp>
#include <log/log.hpp>
#include "client_support.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>
#include <openssl/ssl.h>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using RemoteIo::RemoteErrorKind;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::useTuple;

        RemoteIo::RemoteError cancelled()
        {
            return errorOf(RemoteErrorKind::Cancelled, "The data connection was closed");
        }

        /**
         * @brief Makes the TLS stream over @p socket from @p handle, with an input buffer of @p inputBufferSize where
         *        Asio takes one: it does since 1.38.2, Boost 1.90 brings 1.38.0.
         */
        template <typename StreamT>
        void emplaceTls(
            std::optional<StreamT>& tls,
            asio::ip::tcp::socket& socket,
            SSL* handle,
            [[maybe_unused]] std::size_t inputBufferSize
        )
        {
            if constexpr (std::is_constructible_v<StreamT, asio::ip::tcp::socket&, SSL*, std::size_t>)
                tls.emplace(socket, handle, inputBufferSize);
            else
                tls.emplace(socket, handle);
        }

        /**
         * @brief A failed EPSV that PASV may replace: the server refused or garbled it, or its port could not be
         *        connected, while the control connection is fine.
         */
        bool passiveMayFollow(ControlConnection const& control, RemoteIo::RemoteError const& error)
        {
            return control.state() == ControlState::Open && error.kind != RemoteErrorKind::Cancelled;
        }
    }

    bool PassiveChoice::extendedPassiveFailed() const
    {
        return extendedPassiveFailed_.load(std::memory_order_relaxed);
    }

    void PassiveChoice::rememberExtendedPassiveFailed()
    {
        extendedPassiveFailed_.store(true, std::memory_order_relaxed);
    }

    DataConnection::DataConnection(Private, asio::strand<asio::any_io_executor> strand, DataConnectionOptions options)
        : options_{std::move(options)}
        , strand_{std::move(strand)}
        , socket_{strand_}
    {}

    DataConnection::~DataConnection() = default;

    asio::awaitable<std::expected<std::shared_ptr<DataConnection>, RemoteIo::RemoteError>> DataConnection::open(
        std::shared_ptr<ControlConnection> control,
        DataConnectionOptions options,
        std::shared_ptr<PassiveChoice> choice
    )
    {
        const auto peer = control->remoteEndpoint();
        const bool ipv6 = peer.address().is_v6();
        const auto mode = options.extendedPassive;
        auto data = std::make_shared<DataConnection>(Private{}, control->strand(), options);

        const bool extendedFailedBefore = choice && choice->extendedPassiveFailed();
        if (mode == ExtendedPassive::Always || (mode == ExtendedPassive::Automatic && (ipv6 || !extendedFailedBefore)))
        {
            auto extended = co_await data->openExtended(*control, peer.address());
            if (extended)
                co_return data;
            if (mode == ExtendedPassive::Always || ipv6 || !passiveMayFollow(*control, extended.error()))
                co_return std::unexpected(std::move(extended).error());
            if (choice)
                choice->rememberExtendedPassiveFailed();
            Log::debug("FTP: EPSV did not give a data connection, PASV from now on");
        }
        if (ipv6)
            co_return std::unexpected(
                errorOf(RemoteErrorKind::Unsupported, "PASV cannot name an IPv6 address, EPSV is needed")
            );
        auto passive = co_await data->openPassive(*control, peer.address());
        if (!passive)
            co_return std::unexpected(std::move(passive).error());
        co_return data;
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DataConnection::openExtended(ControlConnection& control, asio::ip::address address)
    {
        const auto reply = co_await control.execute(Command{.verb = Verb::ExtendedPassive});
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(cancelled());
        if (!reply)
            co_return std::unexpected(reply.error());
        const auto port = parseExtendedPassiveReply(*reply);
        if (!port)
            co_return std::unexpected(port.error());
        co_return co_await connectTo(asio::ip::tcp::endpoint{std::move(address), *port});
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DataConnection::openPassive(ControlConnection& control, asio::ip::address peer)
    {
        const auto reply = co_await control.execute(Command{.verb = Verb::Passive});
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return std::unexpected(cancelled());
        if (!reply)
            co_return std::unexpected(reply.error());
        const auto announced = parsePassiveReply(*reply);
        if (!announced)
            co_return std::unexpected(announced.error());
        // RFC 2577 4: the announced address could be another host, the control peer is the one the user chose.
        auto address = options_.trustPassiveAddress ? asio::ip::address{asio::ip::address_v4{announced->address}}
                                                    : std::move(peer);
        co_return co_await connectTo(asio::ip::tcp::endpoint{std::move(address), announced->port});
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
    DataConnection::connectTo(asio::ip::tcp::endpoint endpoint)
    {
        boost::system::error_code ignored{};
        socket_.close(ignored);
        const auto [error] =
            co_await socket_.async_connect(endpoint, asio::cancel_after(options_.connectTimeout, useTuple));
        if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
            co_return std::unexpected(cancelled());
        if (error == asio::error::operation_aborted)
            co_return std::unexpected(errorOf(
                RemoteErrorKind::TimedOut,
                fmt::format("No data connection within {} ms", options_.connectTimeout.count())
            ));
        if (error)
            co_return std::unexpected(errorOf(
                RemoteErrorKind::NoConnection, fmt::format("Could not open the data connection: {}", error.message())
            ));
        remote_ = endpoint;
        boost::system::error_code noDelay{};
        socket_.set_option(asio::ip::tcp::no_delay{true}, noDelay);
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> DataConnection::secure(
        TlsConnectionSettings settings,
        std::optional<SslSessionHandle> session,
        Sha256Digest controlCertificate
    )
    {
        auto handle = makeTlsHandle(settings);
        if (!handle)
            co_return std::unexpected(asNoSecureConnection(std::move(handle).error()));
        if (session && SSL_set_session(handle->get(), session->get()) != 1)
            co_return std::unexpected(
                errorOf(RemoteErrorKind::NoSecureConnection, "The data connection cannot take the control session")
            );
        // The stream takes the handle over.
        emplaceTls(tls_, socket_, handle->release(), options_.tlsInputBufferSize);
        const auto [error] = co_await tls_->async_handshake(
            asio::ssl::stream_base::client, asio::cancel_after(options_.connectTimeout, useTuple)
        );
        if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
            co_return std::unexpected(cancelled());
        if (error)
        {
            auto failure = handshakeError(tls_->native_handle(), error, error == asio::error::operation_aborted);
            failure.kind = RemoteErrorKind::NoSecureConnection;
            failure.message = fmt::format("Data connection: {}", failure.message);
            co_return std::unexpected(std::move(failure));
        }
        resumed_ = SSL_session_reused(tls_->native_handle()) == 1;
        // A resumed session was trusted with the control connection; anything else has to show its certificate.
        if (!resumed_ && peerCertificateDigest(tls_->native_handle()) != controlCertificate)
            co_return std::unexpected(errorOf(
                RemoteErrorKind::NoSecureConnection,
                "The data connection presented another certificate than the control connection"
            ));
        secured_ = true;
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
    DataConnection::readSome(asio::mutable_buffer buffer)
    {
        if (closed_)
            co_return std::unexpected(cancelled());
        const auto token = asio::cancel_after(options_.inactivityTimeout, useTuple);
        const auto [error, size] =
            tls_ ? co_await tls_->async_read_some(buffer, token) : co_await socket_.async_read_some(buffer, token);
        if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
            co_return std::unexpected(cancelled());
        if (!error)
            co_return size;
        if (error == asio::error::eof)
            co_return std::size_t{0u};
        if (error == asio::ssl::error::stream_truncated)
        {
            endedWithoutCloseNotify_ = true;
            co_return std::size_t{0u};
        }
        co_return std::unexpected(transferError(error, error == asio::error::operation_aborted, "Reading"));
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> DataConnection::writeAll(asio::const_buffer buffer)
    {
        if (closed_)
            co_return std::unexpected(cancelled());
        // The inactivity timeout limits each piece, a slow link may take longer for the whole buffer.
        while (buffer.size() > 0u)
        {
            const auto token = asio::cancel_after(options_.inactivityTimeout, useTuple);
            const auto [error, size] = tls_ ? co_await tls_->async_write_some(buffer, token)
                                            : co_await socket_.async_write_some(buffer, token);
            if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
                co_return std::unexpected(cancelled());
            if (error)
                co_return std::unexpected(transferError(error, error == asio::error::operation_aborted, "Writing"));
            buffer += size;
        }
        co_return std::expected<void, RemoteIo::RemoteError>{};
    }

    asio::awaitable<void> DataConnection::finishDownload()
    {
        if (tls_ && !endedWithoutCloseNotify_ && !closed_)
        {
            // The server's close_notify came, the shutdown only sends ours.
            SSL_set_shutdown(tls_->native_handle(), SSL_RECEIVED_SHUTDOWN);
            co_await tls_->async_shutdown(asio::cancel_after(options_.closeNotifyTimeout, useTuple));
            if (isCancelled(co_await asio::this_coro::cancellation_state))
            {
                close();
                co_return;
            }
        }
        close();
    }

    asio::awaitable<std::expected<void, RemoteIo::RemoteError>> DataConnection::finishUpload()
    {
        if (closed_)
            co_return std::unexpected(cancelled());
        if (tls_)
        {
            // Waits for the server's close_notify, which it sends once it took everything.
            const auto [error] =
                co_await tls_->async_shutdown(asio::cancel_after(options_.inactivityTimeout, useTuple));
            if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
                co_return std::unexpected(cancelled());
            close();
            if (error == asio::error::operation_aborted)
                co_return std::unexpected(
                    errorOf(RemoteErrorKind::TimedOut, "The server did not end the upload's TLS in time")
                );
            // A server that closes the socket behind its reply took the bytes as well.
            if (error && error != asio::error::eof && error != asio::ssl::error::stream_truncated)
                co_return std::unexpected(transferError(error, false, "Ending"));
            co_return std::expected<void, RemoteIo::RemoteError>{};
        }

        // Closing before the server took the bytes can reset the connection on Windows, which loses them.
        boost::system::error_code ignored{};
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        boost::system::error_code error{};
        const auto deadline = std::chrono::steady_clock::now() + options_.inactivityTimeout;
        while (!error && std::chrono::steady_clock::now() < deadline)
        {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const auto [readError, size] =
                co_await socket_.async_read_some(asio::buffer(drainBuffer_), asio::cancel_after(left, useTuple));
            error = readError;
        }
        if (isCancelled(co_await asio::this_coro::cancellation_state) || closed_)
            co_return std::unexpected(cancelled());
        close();
        if (error == asio::error::eof)
            co_return std::expected<void, RemoteIo::RemoteError>{};
        // No error left the loop at its deadline.
        if (!error || error == asio::error::operation_aborted)
            co_return std::unexpected(
                errorOf(RemoteErrorKind::TimedOut, "The server did not end the upload's data connection in time")
            );
        co_return std::unexpected(transferError(error, false, "Ending"));
    }

    void DataConnection::close()
    {
        closed_ = true;
        boost::system::error_code ignored{};
        socket_.close(ignored);
    }

    void DataConnection::reset()
    {
        closed_ = true;
        boost::system::error_code ignored{};
        socket_.set_option(asio::socket_base::linger{true, 0}, ignored);
        socket_.close(ignored);
    }

    bool DataConnection::secured() const
    {
        return secured_;
    }

    bool DataConnection::endedWithoutCloseNotify() const
    {
        return endedWithoutCloseNotify_;
    }

    bool DataConnection::resumed() const
    {
        return resumed_;
    }

    asio::ip::tcp::endpoint DataConnection::remoteEndpoint() const
    {
        return remote_;
    }

    RemoteIo::RemoteError
    DataConnection::transferError(boost::system::error_code error, bool timedOut, std::string_view operation) const
    {
        if (timedOut)
            return errorOf(
                RemoteErrorKind::TimedOut, fmt::format("No data moved for {} ms", options_.inactivityTimeout.count())
            );
        return errorOf(
            RemoteErrorKind::ConnectionLost,
            fmt::format("{} the data connection failed: {}", operation, error.message())
        );
    }
}
