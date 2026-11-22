#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace FileTransferProtocol
{
    /**
     * @brief Counts the posts and coroutines of one FTP session that are in flight, so its teardown waits for exactly
     *        those. Asio's own handlers and the client's spawns are not counted, they own only connection, transfer
     *        and pool state. Thread-safe.
     */
    class WorkCounter : public std::enable_shared_from_this<WorkCounter>
    {
      public:
        /** @brief Counts while it lives: what holds one is work in flight. */
        using Token = std::shared_ptr<void>;

        Token acquire();

        /**
         * @return Whether no token was alive within @p limit.
         */
        bool waitIdle(std::chrono::milliseconds limit);

        std::size_t count() const;

      private:
        void releaseOne();

        mutable std::mutex mutex_{};
        std::condition_variable idle_{};
        std::size_t count_{0u};
    };
}
