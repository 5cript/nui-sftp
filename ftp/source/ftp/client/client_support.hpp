#pragma once

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
}
