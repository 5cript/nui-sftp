#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>

#include <utility>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief @p work with throw_if_cancelled(false), for every coroutine that co_spawn starts: a cancel that no
     *        operation under way took must not throw at the next co_await. The cancel reaches the coroutine as the
     *        error of what it cancelled and through its cancellation state.
     */
    template <typename ResultT>
    boost::asio::awaitable<ResultT> withoutThrowing(boost::asio::awaitable<ResultT> work)
    {
        co_await boost::asio::this_coro::throw_if_cancelled(false);
        co_return co_await std::move(work);
    }
}
