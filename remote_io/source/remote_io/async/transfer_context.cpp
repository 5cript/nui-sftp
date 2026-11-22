#include <remote_io/async/transfer_context.hpp>

namespace RemoteIo
{
    AsyncTransferContext::AsyncTransferContext(std::function<void()> wake)
        : wake_{std::move(wake)}
    {}

    bool AsyncTransferContext::end(Outcome outcome, std::optional<RemoteError> error)
    {
        {
            std::scoped_lock lock{endMutex_};
            auto expected = Outcome::Running;
            if (!outcome_.compare_exchange_strong(expected, outcome))
                return false;
            error_ = std::move(error);
        }
        if (observer_)
            observer_();
        return true;
    }

    void AsyncTransferContext::wake() const
    {
        if (wake_)
            wake_();
    }

    void AsyncTransferContext::observe(std::function<void()> observer)
    {
        observer_ = std::move(observer);
    }

    void AsyncTransferContext::reportProgress()
    {
        if (!observer_)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastProgressReport_ < progressInterval)
            return;
        lastProgressReport_ = now;
        observer_();
    }

    void AsyncTransferContext::cancel()
    {
        if (end(Outcome::Canceled))
            wake();
    }

    void AsyncTransferContext::fail(RemoteError error)
    {
        if (end(Outcome::Failed, std::move(error)))
            wake();
    }

    void AsyncTransferContext::complete()
    {
        end(Outcome::Completed);
    }

    bool AsyncTransferContext::hasEnded() const
    {
        return outcome_.load() != Outcome::Running;
    }

    AsyncTransferContext::Outcome AsyncTransferContext::outcome() const
    {
        return outcome_.load();
    }

    std::optional<RemoteError> AsyncTransferContext::error() const
    {
        std::scoped_lock lock{endMutex_};
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
        // Before the transfer shows as resumed, so its first calculation sees the restart.
        if (!doPause)
            restartRateSample();
        paused_.store(doPause);
        if (!doPause)
            wake();
    }

    void AsyncTransferContext::restartRateSample()
    {
        restartRateSample_.store(true);
    }

    void AsyncTransferContext::addBytesTransferred(SignedSizeType delta)
    {
        bytesTransferred_ += delta;
    }

    void AsyncTransferContext::calculateBytesPerSecond()
    {
        calculateBytesPerSecond(std::chrono::steady_clock::now());
    }

    void AsyncTransferContext::calculateBytesPerSecond(std::chrono::steady_clock::time_point now)
    {
        if (restartRateSample_.exchange(false))
        {
            lastBytesTransferred_ = bytesTransferred_.load();
            lastBpsCalculation_ = now;
            return;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBpsCalculation_);
        if (duration > sampleInterval)
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