#pragma once

#include <ftp/client/without_throwing.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief Waits on a timer that only a cancel ends, then awaits a second one.
         *
         * @return Whether the coroutine saw the cancel in its cancellation state.
         */
        boost::asio::awaitable<bool> awaitingBehindACancel()
        {
            namespace asio = boost::asio;
            const auto executor = co_await asio::this_coro::executor;
            asio::steady_timer cancelled{executor, std::chrono::steady_clock::time_point::max()};
            co_await cancelled.async_wait(asio::as_tuple(asio::use_awaitable));
            asio::steady_timer next{executor, std::chrono::milliseconds{0}};
            co_await next.async_wait(asio::as_tuple(asio::use_awaitable));
            const auto state = co_await asio::this_coro::cancellation_state;
            co_return state.cancelled() != asio::cancellation_type::none;
        }

        struct SpawnOutcome
        {
            std::exception_ptr thrown{};
            std::optional<bool> sawCancel{};
        };

        /**
         * @brief Spawns @p work, cancels it at its first wait and runs it to its end.
         */
        SpawnOutcome cancelAtTheFirstWait(boost::asio::awaitable<bool> work)
        {
            namespace asio = boost::asio;
            asio::io_context context{};
            asio::cancellation_signal cancel{};
            SpawnOutcome outcome{};
            asio::co_spawn(
                context,
                std::move(work),
                asio::bind_cancellation_slot(
                    cancel.slot(),
                    [&outcome](std::exception_ptr thrown, bool sawCancel)
                    {
                        outcome.thrown = thrown;
                        if (!thrown)
                            outcome.sawCancel = sawCancel;
                    }
                )
            );
            context.poll();
            cancel.emit(asio::cancellation_type::terminal);
            context.run();
            return outcome;
        }
    }

    TEST(WithoutThrowingTests, ACancelledCoroutineAwaitsOnAndSeesTheCancelInItsState)
    {
        const auto outcome = cancelAtTheFirstWait(Detail::withoutThrowing(awaitingBehindACancel()));

        EXPECT_FALSE(outcome.thrown) << "the await behind the cancel threw";
        ASSERT_TRUE(outcome.sawCancel);
        EXPECT_TRUE(*outcome.sawCancel);
    }

    TEST(WithoutThrowingTests, ACoroutineSpawnedWithoutItThrowsAtTheAwaitBehindACancel)
    {
        // What Asio does by default, and why every coroutine of the client is spawned through withoutThrowing().
        const auto outcome = cancelAtTheFirstWait(awaitingBehindACancel());

        EXPECT_TRUE(outcome.thrown);
    }
}
