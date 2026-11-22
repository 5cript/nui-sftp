#include <ftp/support/executor_probes.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <condition_variable>
#include <future>
#include <mutex>

namespace FileTransferProtocol::Test
{
    namespace asio = boost::asio;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        /** @brief Far above what an executor that runs takes to end the timer. */
        constexpr std::chrono::seconds stopLimit{10};
    }

    struct ExecutorProbes::State
    {
        State(asio::any_io_executor executor, std::chrono::milliseconds probePeriod)
            : target{std::move(executor)}
            , strand{asio::make_strand(target)}
            , timer{strand}
            , period{probePeriod}
        {}

        void recordLateness(Clock::duration lateness)
        {
            const std::lock_guard lock{mutex};
            worstLateness = std::max(worstLateness, lateness);
            ++ticks;
        }

        void recordWake(Clock::duration latency)
        {
            const std::lock_guard lock{mutex};
            worstWake = std::max(worstWake, latency);
        }

        asio::any_io_executor target;
        asio::strand<asio::any_io_executor> strand;
        asio::steady_timer timer;
        std::chrono::milliseconds period;
        mutable std::mutex mutex{};
        std::condition_variable stopped{};
        bool stopping{false};
        Clock::duration worstLateness{};
        Clock::duration worstWake{};
        std::uint64_t ticks{0u};
        std::promise<void> timerEnded{};
    };

    ExecutorProbes::ExecutorProbes(asio::any_io_executor executor, std::chrono::milliseconds period)
        : state_{std::make_shared<State>(std::move(executor), period)}
    {
        asio::co_spawn(
            state_->strand,
            [](std::shared_ptr<State> state) -> asio::awaitable<void>
            {
                auto next = Clock::now() + state->period;
                auto running = true;
                while (running)
                {
                    state->timer.expires_at(next);
                    co_await state->timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    const auto now = Clock::now();
                    {
                        const std::lock_guard lock{state->mutex};
                        running = !state->stopping;
                    }
                    if (running)
                        state->recordLateness(now - next);
                    next = std::max(next + state->period, now);
                }
                state->timerEnded.set_value();
            }(state_),
            asio::detached
        );
        waker_ = std::thread{[state = state_]()
            {
                std::unique_lock lock{state->mutex};
                while (!state->stopping)
                {
                    lock.unlock();
                    const auto postedAt = Clock::now();
                    asio::post(
                        state->target,
                        [state, postedAt]()
                        {
                            state->recordWake(Clock::now() - postedAt);
                        }
                    );
                    lock.lock();
                    state->stopped.wait_for(
                        lock,
                        state->period,
                        [&state]()
                        {
                            return state->stopping;
                        }
                    );
                }
            }};
    }

    ExecutorProbes::~ExecutorProbes()
    {
        stop();
    }

    void ExecutorProbes::stop()
    {
        {
            const std::lock_guard lock{state_->mutex};
            if (state_->stopping)
                return;
            state_->stopping = true;
        }
        state_->stopped.notify_all();
        if (waker_.joinable())
            waker_.join();
        asio::post(
            state_->strand,
            [state = state_]()
            {
                state->timer.cancel();
            }
        );
        // An executor that was stopped never runs the end of the timer, its probe goes with the executor then.
        state_->timerEnded.get_future().wait_for(stopLimit);
    }

    std::chrono::microseconds ExecutorProbes::worstTimerLateness() const
    {
        const std::lock_guard lock{state_->mutex};
        return std::chrono::duration_cast<std::chrono::microseconds>(state_->worstLateness);
    }

    std::chrono::microseconds ExecutorProbes::worstWakeLatency() const
    {
        const std::lock_guard lock{state_->mutex};
        return std::chrono::duration_cast<std::chrono::microseconds>(state_->worstWake);
    }

    std::uint64_t ExecutorProbes::timerTicks() const
    {
        const std::lock_guard lock{state_->mutex};
        return state_->ticks;
    }
}
