#pragma once

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace FileTransferProtocol::Test
{
    /**
     * @brief Watches whether anything holds the threads of an executor: a timer on it records how late it fires, and
     *        a thread of its own posts to it and records how long each handler waited to run. Both repeat every
     *        period until stop().
     */
    class ExecutorProbes
    {
      public:
        ExecutorProbes(boost::asio::any_io_executor executor, std::chrono::milliseconds period);
        /**
         * @brief stop()
         */
        ~ExecutorProbes();
        ExecutorProbes(ExecutorProbes const&) = delete;
        ExecutorProbes& operator=(ExecutorProbes const&) = delete;
        ExecutorProbes(ExecutorProbes&&) = delete;
        ExecutorProbes& operator=(ExecutorProbes&&) = delete;

        /**
         * @brief Ends both probes and waits for them, the worst values stay. An executor that no longer runs keeps the
         *        timer's probe, the wait for it ends after 10 s.
         */
        void stop();

        std::chrono::microseconds worstTimerLateness() const;
        std::chrono::microseconds worstWakeLatency() const;
        /**
         * @brief How often the timer fired, it proves the probe ran.
         */
        std::uint64_t timerTicks() const;

      private:
        struct State;

        std::shared_ptr<State> state_;
        std::thread waker_{};
    };
}
