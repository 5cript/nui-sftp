#pragma once

#include <remote_io/remote_error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>

namespace RemoteIo
{
    class AsyncTransferContext : public std::enable_shared_from_this<AsyncTransferContext>
    {
      public:
        using SignedSizeType = std::make_signed_t<std::size_t>;

        /**
         * @brief How the transfer ended. Only Completed means every byte was transferred.
         */
        enum class Outcome
        {
            Running,
            Completed,
            Failed,
            Canceled,
        };

        bool paused() const;
        /**
         * @brief Pauses or resumes the transfer and wakes the thread that drives it, which sleeps while every transfer
         *        is paused.
         */
        void pause(bool doPause);
        /**
         * @brief Ends the transfer as canceled unless it already ended, and wakes the thread that drives it.
         */
        void cancel();
        /**
         * @brief Ends the transfer as failed with the given error unless it already ended.
         */
        void fail(RemoteError error);
        /**
         * @brief Ends the transfer as completed unless it already ended.
         */
        void complete();
        SignedSizeType bytesTransferred() const;
        SignedSizeType bytesPerSecond() const;
        bool hasEnded() const;
        Outcome outcome() const;
        /**
         * @brief The error a failed transfer ended with.
         */
        std::optional<RemoteError> error() const;
        void calculateBytesPerSecond();

        /**
         * @brief Calls @p observer once the transfer ended and at most every progressInterval while bytes move,
         *        on the thread that ended or moved them. Set before the transfer's first turn, never changed after.
         */
        void observe(std::function<void()> observer);

        /**
         * @brief Sets how pausing, resuming and canceling wake the thread that drives the transfer. Set by the stream
         *        before the transfer starts, never changed after.
         */
        void setWake(std::function<void()> wake);

        /**
         * @brief Tells the observer of progress unless it heard of some within progressInterval. Called by the
         *        transfer after each turn, on the thread that drives it.
         */
        void reportProgress();

        /** @brief How often an observer hears of progress at most. */
        static constexpr auto progressInterval = std::chrono::milliseconds{100};

        /**
         * @brief Adds to the running bytes-transferred counter. Pair with periodic calls to
         *        @ref calculateBytesPerSecond.
         */
        void addBytesTransferred(SignedSizeType delta);

      private:
        void end(Outcome outcome);
        void wake() const;

      private:
        std::function<void()> wake_{};
        std::function<void()> observer_{};
        std::chrono::steady_clock::time_point lastProgressReport_{};
        std::atomic<Outcome> outcome_{Outcome::Running};
        mutable std::mutex errorMutex_{};
        std::optional<RemoteError> error_{};
        std::atomic_bool paused_{false};
        std::atomic<SignedSizeType> bytesTransferred_{0};
        std::atomic<SignedSizeType> lastBytesTransferred_{0};
        std::chrono::high_resolution_clock::time_point lastBpsCalculation_{std::chrono::high_resolution_clock::now()};
        std::atomic<SignedSizeType> bytesPerSecond_{0};
    };
}
