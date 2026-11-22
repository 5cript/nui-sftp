#pragma once

#include <remote_io/remote_error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>

namespace RemoteIo
{
    class AsyncTransferContext
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

        AsyncTransferContext() = default;
        /**
         * @brief A context that wakes the thread driving the transfer when it resumes, fails or is canceled.
         * @param wake Wakes that thread.
         */
        explicit AsyncTransferContext(std::function<void()> wake);

        bool paused() const;
        /**
         * @brief Pauses or resumes the transfer. Resuming wakes the thread that drives it, which sleeps while every
         *        transfer is paused, and restarts the rate sample.
         */
        void pause(bool doPause);
        /**
         * @brief Ends the transfer as canceled and wakes the thread that drives it, unless it already ended.
         */
        void cancel();
        /**
         * @brief Ends the transfer as failed with the given error and wakes the thread that drives it, unless it
         *        already ended.
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
        /**
         * @brief Takes a new rate from the bytes moved since the last one, once sampleInterval passed. Called on the
         *        thread that drives the transfer.
         */
        void calculateBytesPerSecond();
        /**
         * @brief calculateBytesPerSecond() at the given time.
         */
        void calculateBytesPerSecond(std::chrono::steady_clock::time_point now);
        /**
         * @brief Lets the next rate sample start at the next calculation, so time in which nothing was meant to move
         *        counts for no rate. The rate stays as it is until then.
         */
        void restartRateSample();

        /**
         * @brief Calls @p observer once the transfer ended and at most every progressInterval while bytes move,
         *        on the thread that ended or moved them. Set before the transfer's first turn, never changed after.
         */
        void observe(std::function<void()> observer);

        /**
         * @brief Tells the observer of progress unless it heard of some within progressInterval. Called by the
         *        transfer after each turn, on the thread that drives it.
         */
        void reportProgress();

        /** @brief How often an observer hears of progress at most. */
        static constexpr auto progressInterval = std::chrono::milliseconds{100};
        /** @brief The least time a rate is taken over, shorter samples are too inaccurate. */
        static constexpr auto sampleInterval = std::chrono::milliseconds{500};

        /**
         * @brief Adds to the running bytes-transferred counter. Pair with periodic calls to
         *        @ref calculateBytesPerSecond.
         */
        void addBytesTransferred(SignedSizeType delta);

      private:
        /**
         * @brief Ends the transfer with @p outcome and keeps @p error, unless it already ended.
         * @return Whether this call ended the transfer.
         */
        bool end(Outcome outcome, std::optional<RemoteError> error = std::nullopt);
        void wake() const;

      private:
        const std::function<void()> wake_{};
        std::function<void()> observer_{};
        std::chrono::steady_clock::time_point lastProgressReport_{};
        std::atomic<Outcome> outcome_{Outcome::Running};
        /** @brief Makes ending and keeping the error one step, so an end that lost a race keeps no error. */
        mutable std::mutex endMutex_{};
        std::optional<RemoteError> error_{};
        std::atomic_bool paused_{false};
        std::atomic<SignedSizeType> bytesTransferred_{0};
        std::atomic<SignedSizeType> lastBytesTransferred_{0};
        std::chrono::steady_clock::time_point lastBpsCalculation_{std::chrono::steady_clock::now()};
        std::atomic<SignedSizeType> bytesPerSecond_{0};
        std::atomic_bool restartRateSample_{false};
    };
}
