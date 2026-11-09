#pragma once

#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <remote_io/remote_error.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/use_awaitable.hpp>

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
