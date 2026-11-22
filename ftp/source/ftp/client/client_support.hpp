#pragma once

#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <log/log.hpp>
#include <remote_io/remote_error.hpp>
#include <utility/error_message.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <string>
#include <utility>

namespace FileTransferProtocol::Detail
{
    inline constexpr auto useTuple = boost::asio::as_tuple(boost::asio::use_awaitable);

    inline RemoteIo::RemoteError errorOf(RemoteIo::RemoteErrorKind kind, std::string message)
    {
        return RemoteIo::RemoteError{
            .kind = kind,
            .protocol = RemoteIo::Protocol::Ftp,
            .message = std::move(message),
        };
    }

    /**
     * @brief The completion of a coroutine nothing waits for. An exception it ended with is logged as the end of
     *        @p what, then @p end runs: it ends what the coroutine left halfway, a connection as a rule.
     */
    template <typename EndT>
    auto endingOnException(std::string what, EndT end)
    {
        return [what = std::move(what), end = std::move(end)](std::exception_ptr exception) mutable
        {
            if (!exception)
                return;
            Log::error("FTP: {} ended with an exception: {}", what, Utility::errorMessage(exception));
            end();
        };
    }

    inline bool isCancelled(boost::asio::cancellation_state const& state)
    {
        return state.cancelled() != boost::asio::cancellation_type::none;
    }

    /**
     * @brief A 2yz reply.
     */
    inline bool succeeded(Reply const& reply)
    {
        return classifyReply(reply.code) == ReplyClass::PositiveCompletion;
    }

    /**
     * @brief A 4yz or 5yz reply.
     */
    inline bool refused(Reply const& reply)
    {
        const auto replyClass = classifyReply(reply.code);
        return replyClass == ReplyClass::TransientNegative || replyClass == ReplyClass::PermanentNegative;
    }
}
