#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>

namespace FileTransferProtocol::Detail
{
    /**
     * @brief The answer of a check of the system. The check and the wait for it share one, so it outlives a wait that
     *        was stopped.
     */
    class PlatformAnswer
    {
      public:
        void give(bool trusted)
        {
            {
                std::scoped_lock lock{mutex_};
                trusted_ = trusted;
            }
            answered_.notify_all();
        }

        /**
         * @brief Blocks until the answer was given or @p stop is requested.
         *
         * @return Nothing after a stop request without an answer.
         */
        std::optional<bool> waitUntilGiven(std::stop_token const& stop)
        {
            std::unique_lock lock{mutex_};
            answered_.wait(
                lock,
                stop,
                [this]()
                {
                    return trusted_.has_value();
                }
            );
            return trusted_;
        }

      private:
        std::mutex mutex_{};
        std::condition_variable_any answered_{};
        std::optional<bool> trusted_{};
    };
}
