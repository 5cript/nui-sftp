#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/tls.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace FileTransferProtocol
{
    enum class ExtendedPassive : std::uint8_t
    {
        /** @brief EPSV first, PASV after a refusal or a port that cannot be connected, remembered by PassiveChoice. */
        Automatic,
        /** @brief EPSV only. */
        Always,
        /** @brief PASV only, which needs an IPv4 control connection: an IPv6 one fails as Unsupported. */
        Never,
    };

    /**
     * @brief What a session learned about passive mode, shared by its control connections on any strand.
     */
    class PassiveChoice
    {
      public:
        bool extendedPassiveFailed() const;
        void rememberExtendedPassiveFailed();

      private:
        std::atomic<bool> extendedPassiveFailed_{false};
    };

    struct DataConnectionOptions
    {
        ExtendedPassive extendedPassive{ExtendedPassive::Automatic};
        /**
         * @brief Connects to the address a PASV reply names instead of the control peer. Off: a server could point
         *        the client at another host (RFC 2577 4).
         */
        bool trustPassiveAddress{false};
        std::chrono::milliseconds connectTimeout{15'000};
        /** @brief Longest wait for one read or write of data, and for the end of an upload. */
        std::chrono::milliseconds inactivityTimeout{30'000};
        /** @brief How long a download waits to answer the server's close_notify. */
        std::chrono::milliseconds closeNotifyTimeout{2'000};
        /** @brief Bytes the TLS stream takes from the socket at once, larger than one record speeds up FTPS. */
        std::size_t tlsInputBufferSize{256u * 1024u};
    };

    /**
     * @brief The data connection of one transfer, on the strand of its control connection (RFC 959 3.2: passive,
     *        one per transfer). The caller keeps it while one of its operations runs.
     */
    class DataConnection
    {
        struct Private
        {};

      public:
        /**
         * @brief Asks @p control for a port with EPSV or PASV and connects to it on the control peer. A refused EPSV,
         *        or one whose port cannot be connected, falls back to PASV with ExtendedPassive::Automatic and is
         *        remembered in @p choice. An IPv6 control connection uses EPSV only (RFC 2428).
         *
         * @return The connection. The replyError() of a refused PASV or EPSV, NoConnection for a port that cannot
         *         be connected, TimedOut past the connect timeout, Unsupported for ExtendedPassive::Never on IPv6. The
         *         control connection stays usable.
         */
        static boost::asio::awaitable<std::expected<std::shared_ptr<DataConnection>, RemoteIo::RemoteError>> open(
            std::shared_ptr<ControlConnection> control,
            DataConnectionOptions options,
            std::shared_ptr<PassiveChoice> choice
        );

        DataConnection(
            Private,
            boost::asio::strand<boost::asio::any_io_executor> strand,
            DataConnectionOptions options
        );
        ~DataConnection();
        DataConnection(DataConnection const&) = delete;
        DataConnection& operator=(DataConnection const&) = delete;
        DataConnection(DataConnection&&) = delete;
        DataConnection& operator=(DataConnection&&) = delete;

        /**
         * @brief The TLS handshake of a protected data connection (RFC 4217 9), after the transfer command went out.
         *        It resumes a copy of the control connection's session when there is one; a connection that resumed
         *        nothing has to present @p controlCertificate, the leaf of the control connection.
         *
         * @return NoSecureConnection when the handshake fails or presents another certificate, Cancelled after close().
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> secure(
            TlsConnectionSettings settings,
            std::optional<SslSessionHandle> session,
            Sha256Digest controlCertificate
        );

        /**
         * @brief The next bytes of a download, 0 once the data ended. A TLS stream that ended without close_notify
         *        ends the data too, see endedWithoutCloseNotify().
         *
         * @return TimedOut past the inactivity timeout, ConnectionLost for a reset, Cancelled after close().
         */
        boost::asio::awaitable<std::expected<std::size_t, RemoteIo::RemoteError>>
        readSome(boost::asio::mutable_buffer buffer);

        /**
         * @brief Writes all of @p buffer, errors as for readSome(). The inactivity timeout limits each piece written.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> writeAll(boost::asio::const_buffer buffer);

        /**
         * @brief Ends a download whose data ended: answers the server's close_notify within the close_notify timeout
         *        and closes, so the server sends its final reply (proftpd waits for the close).
         */
        boost::asio::awaitable<void> finishDownload();

        /**
         * @brief Ends an upload so the server sees all of it: close_notify and the server's answer over TLS, which
         *        also reads tickets that would reset the connection when left unread; a shut sending side and the
         *        server's end without TLS. Both within the inactivity timeout, then the socket closes.
         *
         * @return TimedOut when the server did not end its side in time, the error of the shutdown otherwise.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>> finishUpload();

        /**
         * @brief Ends a download cut short while serverMayBeInHandshake(): waits for the server's first bytes or its
         *        end, which it sends only once its handshake ended, within the inactivity timeout, then closes.
         */
        boost::asio::awaitable<void> closeOnceAnswered();

        /**
         * @brief Closes at once, operations under way end as Cancelled.
         */
        void close();

        /**
         * @brief Closes with a TCP reset, so a server cannot take a cut upload for a complete one.
         */
        void reset();

        /**
         * @brief The handshake of secure() ended and the certificate was checked.
         */
        bool secured() const;

        /**
         * @brief The TLS stream ended with a TCP close and no close_notify: the data may be cut, only a positive final
         *        reply together with the expected size makes it complete.
         */
        bool endedWithoutCloseNotify() const;

        /**
         * @brief The handshake of secure() resumed the control connection's session.
         */
        bool resumed() const;

        /**
         * @brief Nothing was read or written since the TLS handshake, so the server may still finish its side of it. A
         *        reset then can lose the client's last handshake message, and proftpd ends the whole session for a data
         *        handshake that failed.
         */
        bool serverMayBeInHandshake() const;

        boost::asio::ip::tcp::endpoint remoteEndpoint() const;

      private:
        /**
         * @brief EPSV and a connect to its port on @p address.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        openExtended(ControlConnection& control, boost::asio::ip::address address);
        /**
         * @brief PASV and a connect to its port on @p peer, or on the announced address when that is trusted.
         */
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        openPassive(ControlConnection& control, boost::asio::ip::address peer);
        boost::asio::awaitable<std::expected<void, RemoteIo::RemoteError>>
        connectTo(boost::asio::ip::tcp::endpoint endpoint);
        RemoteIo::RemoteError
        transferError(boost::system::error_code error, bool timedOut, std::string_view operation) const;

        DataConnectionOptions options_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        boost::asio::ip::tcp::socket socket_;
        std::optional<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> tls_{};
        boost::asio::ip::tcp::endpoint remote_{};
        /** @brief Takes what the server sends after an upload, until its end. */
        std::array<char, 512> drainBuffer_{};
        bool endedWithoutCloseNotify_{false};
        bool secured_{false};
        bool resumed_{false};
        bool closed_{false};
        /** @brief Something was read or the data ended since the handshake. */
        bool answered_{false};
        bool wrote_{false};
    };
}
