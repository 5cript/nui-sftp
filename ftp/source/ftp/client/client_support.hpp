#pragma once

#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <remote_io/remote_error.hpp>

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
     * @brief What @p exception says, for the log and the error of what it ended.
     */
    inline std::string describe(std::exception_ptr exception)
    {
        try
        {
            std::rethrow_exception(exception);
        }
        catch (std::exception const& error)
        {
            return error.what();
        }
        catch (...)
        {
            return "an exception of an unknown type";
        }
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
