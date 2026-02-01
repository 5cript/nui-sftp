#pragma once

#include <ssh/async/processing_thread.hpp>

#include <memory>
#include <atomic>

namespace SecureShell
{
    class FileStream;

    class AsyncTransferContext : public std::enable_shared_from_this<AsyncTransferContext>
    {
      public:
        using SignedSizeType = std::make_signed_t<std::size_t>;

        // may update bytesTransferred_
        friend FileStream;

        bool paused() const;
        void pause(bool doPause);
        void cancel();
        SignedSizeType bytesTransferred() const;
        SignedSizeType bytesPerSecond() const;
        bool hasEnded() const;
        void calculateBytesPerSecond();

      private:
        std::atomic_bool ended_{false};
        std::atomic_bool paused_{false};
        std::atomic<SignedSizeType> bytesTransferred_{0};
        std::atomic<SignedSizeType> lastBytesTransferred_{0};
        std::chrono::high_resolution_clock::time_point lastBpsCalculation_{std::chrono::high_resolution_clock::now()};
        ProcessingThread::PermanentTaskId permanentTaskId_{-1};
        std::atomic<SignedSizeType> bytesPerSecond_{0};
    };
}