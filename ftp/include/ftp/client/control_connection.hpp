#pragma once

#include <ftp/client/pending_command.hpp>
#include <ftp/client/socket_options.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace FileTransferProtocol
{
    struct ControlConnectionOptions
    {
        /** @brief The TCP connect and the greeting together, a 120 before the 220 included. */
        std::chrono::milliseconds connectTimeout{15'000};
        /**
         * @brief From writing a command to its first reply. A transfer has no limit after its preliminary reply, see
         *        ControlConnection::limitFinalReply().
         */
        std::chrono::milliseconds commandTimeout{15'000};
        /**
         * @brief From writing ABOR into a transfer to the reply of its marker. A server that misses it ends the
         *        connection as Dead.
         */
        std::chrono::milliseconds drainTimeout{5'000};
        /** @brief Quiet time after which a line with another code ends a multi-line reply, see ReplyFramer::settle().
         */
        std::chrono::milliseconds settleTime{200};
        /** @brief How long a close over TLS may take to send close_notify, the server's is not awaited. */
        std::chrono::milliseconds closeNotifyTimeout{2'000};
        ControlSocketOptions socket{};
        ReplyLimits replyLimits{};
    };

    enum class ControlState : std::uint8_t
    {
        Unconnected,
        Open,
        /** @brief Closed by the client, or by the server after the reply to QUIT. */
        Closed,
        /** @brief Ended by a failure, see ControlConnection::failure(). */
        Dead,
    };

    /**
     * @brief The control connection of an FTP session. One reader keeps a read pending for as long as the connection
     *        is open, also while it is idle, so a 421, an EOF or a reset is seen at once. Replies go to the commands
     *        in the order the commands were written.
     *
     * A command is written once every earlier one has its first reply (RFC 959 5.4) and has completed. Only ABOR goes
     * into a transfer that got its preliminary reply (RFC 959 4.2), ahead of commands queued before it. STAT and QUIT
     * wait like any other command: servers answer them before the final reply of the transfer, so a session that ends
     * during a transfer aborts it first or closes. A connection whose replies cannot be matched to its commands any
     * more ends as Dead: a reply nothing waits for, a 421, a reply past its deadline, a framing error.
     *
     * ABOR into a transfer is followed by PWD as a marker. Servers answer the two with two replies in either order,
     * with one, or with none; the final replies before the 257 of PWD go to the transfer and ABOR: the first to the
     * transfer, the last to ABOR, a single one to both. The drain ends at the marker's reply within drainTimeout.
     *
     * TLS starts before the greeting (implicit) or after a 234 to AUTH TLS (explicit, RFC 4217), and stays to the end:
     * a close sends close_notify, CCC is never sent. The certificate is checked during the handshake, so no command
     * goes out before the decision: trusted authorities first, then the pin. A connection that was to get TLS either
     * gets it or ends, it never goes on in plaintext.
     *
     * Everything but close() runs on strand(), and all of it stays there: the reader, the timers, the waits. Its
     * operations hold the connection weakly and own only their buffers, commands and the socket with its TLS stream:
     * the owner decides how long it lives, dropping the last shared_ptr closes it, and waiting commands fail as
     * Cancelled. A caller of connect() or startTls() keeps the connection while it waits.
     */
    class ControlConnection : public std::enable_shared_from_this<ControlConnection>
    {
        struct Private
        {};

      public:
        using EndHandler = std::function<void(RemoteIo::RemoteError const&)>;

        /**
         * @param executor Any executor, the connection makes a strand of its own on it.
         */
        static std::shared_ptr<ControlConnection>
        create(boost::asio::any_io_executor executor, ControlConnectionOptions options = {});

        ControlConnection(Private, boost::asio::any_io_executor executor, ControlConnectionOptions options);
        /**
         * @brief Closes the socket, on whatever thread the last owner let go. Waiting commands fail on strand().
         */
        ~ControlConnection();
        ControlConnection(ControlConnection const&) = delete;
        ControlConnection& operator=(ControlConnection const&) = delete;
        ControlConnection(ControlConnection&&) = delete;
        ControlConnection& operator=(ControlConnection&&) = delete;

        boost::asio::strand<boost::asio::any_io_executor> const& strand() const;

        /**
         * @brief Connects to the first of @p endpoints that accepts within the connect timeout, starts the reader and
         *        waits for the greeting. Each endpoint gets its share of the time left, one that never answers leaves
         *        time for the rest.
         *
         * @param implicitTls Makes the TLS handshake before the greeting, within the connect timeout.
         * @return The 220 greeting. NoConnection when no endpoint accepted, TimedOut past the connect timeout,
         *         Cancelled by close(), also one before connect(), or a cancelled wait, the replyError() of any other
         *         greeting. With implicitTls TrustRequired for a certificate to ask about, the replyError() of a
         *         negative reply the server sent in plaintext instead, and NoSecureConnection for any other failure
         *         of the handshake, a greeting in plaintext included. The connection is Dead after an error, Closed
         *         after close().
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> connect(
            std::vector<boost::asio::ip::tcp::endpoint> endpoints,
            std::optional<TlsConnectionSettings> implicitTls = std::nullopt
        );

        /**
         * @brief Sends AUTH TLS on the idle plain connection and makes the handshake after a 234, or a 334 as curl
         *        accepts it. Nothing may follow that reply in plaintext: such bytes could have been injected before
         *        the handshake. The handshake has the command timeout. Every failure ends the connection, it never
         *        goes on in plaintext.
         *
         * @return Who vouched for the certificate. TrustRequired for a certificate to ask about, Cancelled after a
         *         cancel or close(), NoSecureConnection with the reason for any other failure, a refused AUTH and bytes
         *         behind its reply included. Only a Failure for a connection that has or starts TLS already leaves it
         *         as it is.
         */
        boost::asio::awaitable<std::expected<TrustedCertificate, RemoteIo::RemoteError>>
        startTls(TlsConnectionSettings settings);

        /**
         * @brief Queues @p command, it is written as soon as the commands before it allow.
         *
         * @return The command, whose replies the caller takes. InvalidPath for an argument that cannot be sent, the
         *         connection stays usable. The error of the connection when it is not open. A Failure for AUTH, which
         *         goes through startTls(), and for any command while TLS starts.
         */
        std::expected<std::shared_ptr<PendingCommand>, RemoteIo::RemoteError> send(Command command);

        /**
         * @brief send() and its final reply, preliminary replies skipped. A cancelled wait withdraws the command when
         * it was not written yet.
         *
         * @return The final reply whatever its code, judging it is the caller's part. Errors as for send() and
         *         PendingCommand::nextReply().
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> execute(Command command);

        /**
         * @brief Removes a command that was not written yet, it fails as Cancelled. A written command keeps its place,
         *        its replies are read and dropped.
         *
         * @return Whether the command was removed.
         */
        bool withdraw(PendingCommand const& pending);

        /**
         * @brief The final reply of @p pending has to come within @p timeout from now, or the connection is Dead. For
         *        the end of a transfer, once its data is through. A preliminary reply that comes later keeps the limit.
         */
        void limitFinalReply(PendingCommand& pending, std::chrono::milliseconds timeout);

        /**
         * @brief Waits until no command is queued or owes its final reply, the drain of an ABOR included, or the
         *        connection ended. For a connection nobody holds anymore: one that is still busy after the command
         *        timeout plus the drain timeout ends as Dead, a transfer nobody finishes has no deadline of its own.
         *        On strand().
         */
        boost::asio::awaitable<void> untilIdle();

        /**
         * @brief Closes the socket without QUIT. Waiting commands fail as Cancelled. Callable from any thread.
         */
        void close();

        /**
         * @brief Called on strand() once, when the connection ends while it is still owned, with the error waiting
         *        commands got.
         */
        void onEnd(EndHandler handler);

        ControlState state() const;

        /**
         * @brief Why the connection is Dead, nothing while it is not.
         */
        std::optional<RemoteIo::RemoteError> const& failure() const;

        /**
         * @brief The peer of the control connection, which passive data connections go to (RFC 2577).
         */
        boost::asio::ip::tcp::endpoint remoteEndpoint() const;

        /**
         * @brief The client's end of the control connection, set by connect().
         */
        boost::asio::ip::tcp::endpoint localEndpoint() const;

        /**
         * @brief The certificate of the handshake and who vouched for it, nothing without TLS.
         */
        std::optional<TrustedCertificate> const& trustedCertificate() const;

        /**
         * @brief The certificate a failed handshake refused, what a trust prompt shows.
         */
        std::optional<CertificateInfo> const& untrustedCertificate() const;

        /**
         * @brief A copy of the TLS session for one data connection to resume, see resumableSession(). Nothing without
         *        TLS and while no resumable session arrived.
         */
        std::optional<SslSessionHandle> sessionForData();

      private:
        class Transport;

        /**
         * @brief ABOR written into a transfer with its marker behind it, and the final replies that came before the
         *        marker's.
         */
        struct AbortDrain
        {
            std::shared_ptr<PendingCommand> transfer{};
            std::shared_ptr<PendingCommand> abort{};
            std::shared_ptr<PendingCommand> marker{};
            std::vector<Reply> replies{};
        };

        boost::asio::awaitable<std::expected<boost::asio::ip::tcp::endpoint, RemoteIo::RemoteError>> connectToFirst(
            std::vector<boost::asio::ip::tcp::endpoint> const& endpoints,
            std::chrono::steady_clock::time_point deadline
        );
        /**
         * @brief The handshake of @p handle on the connected socket, the connection is Dead when it fails.
         *
         * @param plaintextRefusal A negative reply in plaintext instead of the handshake is the server's answer, the
         *        replyError() of it is the failure. Only before the greeting of implicit TLS, behind AUTH TLS such
         *        bytes could be injected.
         */
        boost::asio::awaitable<std::expected<TrustedCertificate, RemoteIo::RemoteError>>
        handshake(SslHandle handle, std::chrono::steady_clock::time_point deadline, bool plaintextRefusal);
        /**
         * @brief Ends the connection after a failed start of TLS.
         *
         * @return What startTls() reports: Cancelled after close(), else asNoSecureConnection() of @p error.
         */
        RemoteIo::RemoteError endTls(RemoteIo::RemoteError error);
        std::expected<std::shared_ptr<PendingCommand>, RemoteIo::RemoteError> enqueue(Command command);
        void readNext();
        void takeRead(boost::system::error_code error, std::string_view bytes);
        void takeBytes(std::string_view bytes);
        void deliverFramedReplies();
        void dispatch(Reply reply);
        void takeDrainedReply(Reply reply);
        void finishDrain(Reply markerReply);
        std::shared_ptr<PendingCommand> makeMarker() const;
        void takeEndOfStream(boost::system::error_code error);
        /**
         * @brief Why a read failed, the server's reply when it wrote one in plaintext into TLS.
         */
        RemoteIo::RemoteError readError(boost::system::error_code error) const;
        void writeNext();
        std::deque<std::shared_ptr<PendingCommand>>::iterator nextToWrite();
        /**
         * @brief The transfer commands may go into: every written command has its first reply and one is a transfer.
         */
        std::shared_ptr<PendingCommand> runningTransfer() const;
        void armDeadline();
        void takeDeadline();
        void armSettle();
        void wakeIdleWaiters();
        void die(RemoteIo::RemoteError error);
        void end(ControlState state, RemoteIo::RemoteError error);
        bool isLive() const;
        static std::optional<std::chrono::steady_clock::time_point>
        deadlineAfterPreliminary(PendingCommand const& pending);

        ControlConnectionOptions options_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        /** @brief Shared with the operations in flight, an ssl::stream must outlive them. */
        std::shared_ptr<Transport> transport_;
        boost::asio::ip::tcp::endpoint remote_{};
        boost::asio::ip::tcp::endpoint local_{};
        ReplyFramer framer_;
        boost::asio::steady_timer deadlineTimer_;
        /** @brief What deadlineTimer_ waits for, an unchanged earliest deadline is not armed again. */
        std::optional<std::chrono::steady_clock::time_point> armedDeadline_{};
        boost::asio::steady_timer settleTimer_;
        /** @brief Never expires, a cancel wakes the waits of untilIdle(). */
        boost::asio::steady_timer idleSignal_;
        /** @brief Shared with the read in flight, which may still fill it after the connection is gone. */
        std::shared_ptr<std::array<char, 4096>> readBuffer_;
        /** @brief Commands not written yet, in the order they were sent. */
        std::deque<std::shared_ptr<PendingCommand>> queued_{};
        /** @brief Written commands that still owe their final reply, oldest first. */
        std::deque<std::shared_ptr<PendingCommand>> awaiting_{};
        std::optional<AbortDrain> drain_{};
        bool writing_{false};
        /** @brief A read is under way. */
        bool reading_{false};
        /** @brief TLS starts, implicitly or through startTls(), no command may be sent. */
        bool upgrading_{false};
        /** @brief AUTH TLS was accepted, the bytes behind its reply belong to the handshake. */
        bool readerPaused_{false};
        std::optional<TrustedCertificate> trustedCertificate_{};
        std::optional<CertificateInfo> untrustedCertificate_{};
        ControlState state_{ControlState::Unconnected};
        std::optional<RemoteIo::RemoteError> failure_{};
        EndHandler endHandler_{};
    };
}
