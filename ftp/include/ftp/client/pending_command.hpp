#pragma once

#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>

namespace FileTransferProtocol
{
    class ControlConnection;

    /**
     * @brief A command on a control connection and the replies it is owed: preliminary 1yz replies, then one final
     *        reply (RFC 959 4.2). Used on the strand of its connection only. Waits may overlap, each reply goes to the
     *        wait that takes it first.
     */
    class PendingCommand
    {
      public:
        enum class Stage : std::uint8_t
        {
            /** @brief Waits behind earlier commands, see ControlConnection::send(). */
            Queued,
            /** @brief Sent, no reply yet. */
            Written,
            /** @brief A preliminary reply came. */
            Answered,
            /** @brief The final reply came, or the command failed. */
            Completed,
        };

        /** @brief More undelivered replies than this break the protocol. */
        static constexpr std::size_t maximumHeldReplies = 16u;

        /**
         * @param command Nothing for the greeting, which the server sends unasked.
         * @param bytes What is written, see Command::encode().
         */
        PendingCommand(boost::asio::any_io_executor strand, std::optional<Command> command, std::string bytes);

        /**
         * @brief The next reply, preliminary or final, in the order they came.
         *
         * @return The error that ended the connection or withdrew the command, Cancelled when the wait was cancelled,
         *         a Failure when the final reply was taken already.
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> nextReply();

        /**
         * @brief The final reply, preliminary ones skipped. Errors as for nextReply().
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> finalReply();

        /**
         * @brief Nothing for the greeting.
         */
        std::optional<Verb> verb() const;

        Stage stage() const;

        /**
         * @brief A preliminary or the final reply came, so commands behind it may be written.
         */
        bool answered() const;

      private:
        friend class ControlConnection;

        enum class Wanted : std::uint8_t
        {
            Next,
            Final,
        };

        /**
         * @brief The wait of nextReply() and finalReply() in one coroutine: after a cancelled wait it awaits nothing
         *        more, Asio would throw operation_aborted.
         */
        boost::asio::awaitable<std::expected<Reply, RemoteIo::RemoteError>> takeReply(Wanted wanted);
        void take(Reply reply);
        void fail(RemoteIo::RemoteError error);
        void wake();

        std::optional<Command> command_;
        std::string bytes_;
        boost::asio::steady_timer signal_;
        std::deque<Reply> replies_{};
        std::optional<RemoteIo::RemoteError> failure_{};
        Stage stage_{Stage::Queued};
        bool finalTaken_{false};
        /** @brief When the connection gives up waiting for the next reply, nothing for no limit. */
        std::optional<std::chrono::steady_clock::time_point> deadline_{};
        /** @brief Set by ControlConnection::limitFinalReply(), a preliminary reply after it does not lift it. */
        std::optional<std::chrono::steady_clock::time_point> finalDeadline_{};
    };
}
