#pragma once

#include <ftp/client/control_connection.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/protocol/features.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace FileTransferProtocol
{
    struct Credentials
    {
        std::string user{};
        std::string password{};
        /** @brief Sent with ACCT when the server asks for an account (332), the login fails without one then. */
        std::optional<std::string> account{};
    };

    struct LoginOptions
    {
        TlsMode tlsMode{TlsMode::None};
        /** @brief How the server's certificate is checked, needed unless tlsMode is None. */
        std::optional<TlsConnectionSettings> tls{};
        Credentials credentials{};
    };

    /**
     * @brief What a login learned about the server.
     */
    struct LoginResult
    {
        Reply greeting{};
        /** @brief Empty when the server does not know FEAT. */
        Features features{};
        /** @brief The directory the login starts in, from PWD. */
        std::string homeDirectory{};
        /** @brief The server took OPTS UTF8 ON, which servers that use UTF-8 anyway may refuse. */
        bool utf8Accepted{false};
        /** @brief PROT P was accepted, data connections use TLS. */
        bool dataProtected{false};
        /** @brief The server's certificate and who vouched for it, nothing without TLS. */
        std::optional<TrustedCertificate> certificate{};
    };

    /**
     * @brief Connects @p connection and logs in: the greeting (implicit TLS before it), AUTH TLS, USER, PASS, ACCT
     *        when asked for, PBSZ 0 and PROT P with TLS, FEAT, OPTS UTF8 ON, TYPE I and PWD. The credentials go out
     *        only after the trust decision, and never in plaintext when TLS was asked for. PBSZ and PROT follow the
     *        login as with curl and FileZilla. Runs on the strand of @p connection, which the caller keeps.
     *
     * @return The result, or the error of the step that failed: a replyError() of the server's reply,
     *         TrustRequired with ControlConnection::untrustedCertificate() to ask about, NoSecureConnection when TLS
     *         or the protection of the data connections failed, Cancelled also when a reply came with the cancel. The
     *         connection is closed after an error.
     */
    boost::asio::awaitable<std::expected<LoginResult, RemoteIo::RemoteError>> logIn(
        std::shared_ptr<ControlConnection> connection,
        std::vector<boost::asio::ip::tcp::endpoint> endpoints,
        LoginOptions options
    );
}
