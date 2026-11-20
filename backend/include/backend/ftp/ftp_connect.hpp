#pragma once

#include <backend/ftp/ftp_engine_context.hpp>
#include <backend/session.hpp>
#include <ftp/client/login.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/remote/ftp_connection.hpp>
#include <persistence/state/ftp_options.hpp>
#include <persistence/state/session_options.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief How the backend logs in to an FTP server: the saved options of the session as options of the client, the
 *        password, the login and why it failed.
 */
namespace FtpConnect
{
    /**
     * @brief Asks for the password with the text of a prompt, nothing when the prompt was cancelled.
     */
    using PasswordSource = std::function<std::optional<std::string>(std::string const& prompt)>;

    /**
     * @brief Whether @p user logs in anonymously, without a password: no name, or "anonymous" or "ftp" in any case.
     */
    bool isAnonymous(Persistence::FtpUser const& user);

    /**
     * @brief The text of the password prompt, "Password for alice@ftp.example.com:".
     */
    std::string passwordPrompt(std::string const& user, std::string const& host);

    /**
     * @brief The credentials of @p user. The password of an anonymous login is filled in, any other is empty.
     */
    FileTransferProtocol::Credentials credentialsOf(Persistence::FtpUser const& user);

    /**
     * @brief The options of the TLS context of a session: the authorities of the system and the minimum version.
     */
    FileTransferProtocol::TlsContextOptions tlsContextOptionsOf(Persistence::FtpOptions const& options);

    /**
     * @brief The client options of @p session, whose references are resolved, logging in as its loginUser() with
     *        credentialsOf() it. Unset options keep the defaults of the client. A timeout raised above its default
     *        moves the backstops of the operations and the stream calls back by as much, so they end only what hangs.
     *
     * @param tlsContext Checks the certificate of the server, needed unless the session is plain.
     * @return The options, or which saved option is out of range.
     */
    std::expected<FileTransferProtocol::FtpConnectionOptions, std::string> connectionOptionsOf(
        Persistence::FtpSessionOptions const& session,
        std::shared_ptr<FileTransferProtocol::TlsContext> tlsContext
    );

    /**
     * @brief Why the login to the server of @p session failed, in words for the user. A certificate that is not
     *        trusted comes with its SHA-256 fingerprint.
     */
    std::string loginFailureMessage(
        FileTransferProtocol::LoginFailure const& failure,
        Persistence::FtpSessionOptions const& session
    );

    /**
     * @brief Logs in to the server of @p session on the threads of @p engine, asking @p askPassword unless the login is
     *        anonymous. Blocks until the login ended, within the login limits of the connection. Neither on an engine
     *        thread nor on a processing thread.
     *
     * @param onConnectionLoss Called once when the session is lost after the start of the connection, on its
     *        processing thread, or from within start() for a loss before it.
     * @return The logged in connection, or why the connect failed.
     */
    std::expected<SessionConnection, std::string> connect(
        FtpEngineContext& engine,
        Persistence::FtpSessionOptions const& session,
        PasswordSource const& askPassword,
        std::function<void()> onConnectionLoss
    );
}
