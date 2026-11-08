#pragma once

#include <ftp/client/pending_command.hpp>
#include <ftp/client/socket_options.hpp>
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
         * @brief From writing a command to its first reply, and from a preliminary reply to the next. A transfer has
         *        no limit after its preliminary reply, see ControlConnection::limitFinalReply().
         */
        std::chrono::milliseconds commandTimeout{15'000};
        /** @brief Quiet time after which a line with another code ends a multi-line reply, see ReplyFramer::settle().
         */
        std::chrono::milliseconds settleTime{200};
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
     * A command is written once every earlier one has its first reply (RFC 959 5.4) and has completed, except ABOR,
     * STAT and QUIT, which may interrupt a transfer (RFC 959 4.2). A connection whose replies cannot be matched to its
     * commands any more ends as Dead: a reply nothing waits for, a 421, a reply past its deadline, a framing error.
     *
     * Everything but close() runs on strand(), and all of it stays there: the reader, the timers, the waits.
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
        ~ControlConnection();
        ControlConnection(ControlConnection const&) = delete;
        ControlConnection& operator=(ControlConnection const&) = delete;
        ControlConnection(ControlConnection&&) = delete;
        ControlConnection& operator=(ControlConnection&&) = delete;

        boost::asio::strand<boost::asio::any_io_executor> const& strand() const;

        /**
         * @brief Connects to the first of @p endpoints that accepts within the connect timeout, starts the reader and
         *        waits for the greeting.
         *
         * @return The 220 greeting. NoConnection when no endpoint accepted, TimedOut past the connect timeout, the
         *         replyError() of any other greeting. The connection is Dead after an error.
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>>
        connect(std::vector<boost::asio::ip::tcp::endpoint> endpoints);

        /**
         * @brief Queues @p command, it is written as soon as the commands before it allow.
         *
         * @return The command, whose replies the caller takes. InvalidPath for an argument that cannot be sent, the
         *         connection stays usable. The error of the connection when it is not open.
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
         *        the end of a transfer, once its data is through.
         */
        void limitFinalReply(PendingCommand& pending, std::chrono::milliseconds timeout);

        /**
         * @brief Closes the socket without QUIT. Waiting commands fail as Cancelled. Callable from any thread.
         */
        void close();

        /**
         * @brief Called on strand() once, when the connection ends for whatever reason, with the error waiting
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

      private:
        boost::asio::awaitable<void> readReplies(std::shared_ptr<ControlConnection> self);
        void takeBytes(std::string_view bytes);
        void deliverFramedReplies();
        void dispatch(Reply reply);
        void takeEndOfStream(boost::system::error_code error);
        void writeNext();
        void armDeadline();
        void takeDeadline();
        void armSettle();
        void die(RemoteIo::RemoteError error);
        void end(ControlState state, RemoteIo::RemoteError error);
        bool isLive() const;
        std::optional<std::chrono::steady_clock::time_point> deadlineAfterPreliminary(PendingCommand const& pending);

        ControlConnectionOptions options_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        boost::asio::ip::tcp::socket socket_;
        boost::asio::ip::tcp::endpoint remote_{};
        ReplyFramer framer_;
        boost::asio::steady_timer deadlineTimer_;
        boost::asio::steady_timer settleTimer_;
        std::array<char, 4096> readBuffer_{};
        /** @brief Commands not written yet, in the order they were sent. */
        std::deque<std::shared_ptr<PendingCommand>> queued_{};
        /** @brief Written commands that still owe their final reply, oldest first. */
        std::deque<std::shared_ptr<PendingCommand>> awaiting_{};
        bool writing_{false};
        ControlState state_{ControlState::Unconnected};
        std::optional<RemoteIo::RemoteError> failure_{};
        EndHandler endHandler_{};
    };
}
