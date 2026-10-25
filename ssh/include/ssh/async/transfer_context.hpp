#pragma once

#include <ssh/async/processing_thread.hpp>
#include <ssh/sftp_error.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace SecureShell
{
    class FileStream;

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

        // may update bytesTransferred_
        friend FileStream;

        bool paused() const;
        /**
         * @brief Pauses or resumes the transfer; resuming wakes the processing thread, which sleeps while
         *        every transfer is paused.
         */
        void pause(bool doPause);
        /**
         * @brief Ends the transfer as canceled unless it already ended.
         */
        void cancel();
        /**
         * @brief Ends the transfer as failed with the given error unless it already ended.
         */
        void fail(SftpError error);
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
        std::optional<SftpError> error() const;
        void calculateBytesPerSecond();

        /**
         * @brief Add to the running bytes-transferred counter from outside the
         *        async FileStream path (e.g. a sync read-loop driven by an
         *        Operation that still wants the throughput-meter machinery).
         *        Pair with periodic calls to @ref calculateBytesPerSecond.
         */
        void addBytesTransferred(SignedSizeType delta);

      private:
        void end(Outcome outcome);

      private:
        /** @brief Set by FileStream before the transfer starts, never changed after. */
        std::function<void()> wake_{};
        std::atomic<Outcome> outcome_{Outcome::Running};
        mutable std::mutex errorMutex_{};
        std::optional<SftpError> error_{};
        std::atomic_bool paused_{false};
        std::atomic<SignedSizeType> bytesTransferred_{0};
        std::atomic<SignedSizeType> lastBytesTransferred_{0};
        std::chrono::high_resolution_clock::time_point lastBpsCalculation_{std::chrono::high_resolution_clock::now()};
        ProcessingThread::PermanentTaskId permanentTaskId_{-1};
        std::atomic<SignedSizeType> bytesPerSecond_{0};
    };
}
