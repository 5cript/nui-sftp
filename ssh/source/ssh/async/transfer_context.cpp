#include <ssh/async/transfer_context.hpp>

namespace SecureShell
{
    void AsyncTransferContext::end(Outcome outcome)
    {
        auto expected = Outcome::Running;
        outcome_.compare_exchange_strong(expected, outcome);
        ended_.store(true);
    }

    void AsyncTransferContext::cancel()
    {
        end(Outcome::Canceled);
    }

    void AsyncTransferContext::fail(SftpError error)
    {
        if (outcome_.load() == Outcome::Running)
        {
            std::scoped_lock lock{errorMutex_};
            if (!error_)
                error_ = std::move(error);
        }
        end(Outcome::Failed);
    }

    void AsyncTransferContext::complete()
    {
        end(Outcome::Completed);
    }

    bool AsyncTransferContext::hasEnded() const
    {
        return ended_.load();
    }

    AsyncTransferContext::Outcome AsyncTransferContext::outcome() const
    {
        return outcome_.load();
    }

    std::optional<SftpError> AsyncTransferContext::error() const
    {
        std::scoped_lock lock{errorMutex_};
        return error_;
    }

    AsyncTransferContext::SignedSizeType AsyncTransferContext::bytesPerSecond() const
    {
        return bytesPerSecond_.load();
    }

    AsyncTransferContext::SignedSizeType AsyncTransferContext::bytesTransferred() const
    {
        return bytesTransferred_.load();
    }

    bool AsyncTransferContext::paused() const
    {
        return paused_.load();
    }

    void AsyncTransferContext::pause(bool doPause)
    {
        paused_.store(doPause);
    }

    void AsyncTransferContext::addBytesTransferred(SignedSizeType delta)
    {
        bytesTransferred_ += delta;
    }

    void AsyncTransferContext::calculateBytesPerSecond()
    {
        using namespace std::chrono_literals;
        const auto now = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBpsCalculation_);
        // Take at least 500ms between calculations to increase accuracy
        if (duration > 500ms)
        {
            const auto currentBytes = bytesTransferred_.load();
            const auto bytesThisCycle = currentBytes - lastBytesTransferred_.load();
            const auto bps = (bytesThisCycle * 1000) / duration.count();
            bytesPerSecond_ = bps;
            lastBytesTransferred_ = currentBytes;
            lastBpsCalculation_ = now;
        }
    }
}