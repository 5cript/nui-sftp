#pragma once

#include <future>
#include <atomic>

class Awaiter
{
  public:
    Awaiter(int maxCount = 1)
        : maxCount_(maxCount)
    {}

    bool waitFor(std::chrono::milliseconds const& duration = std::chrono::seconds{5})
    {
        return promise_.get_future().wait_for(duration) == std::future_status::ready;
    }

    void wait()
    {
        promise_.get_future().wait();
    }

    void reset()
    {
        counter_ = 0;
        promise_ = std::promise<void>{};
    }

    void arrive()
    {
        ++counter_;
        if (counter_ == maxCount_)
        {
            promise_.set_value();
        }
    }

  private:
    const int maxCount_{0};
    std::atomic_int counter_ = 0;
    std::promise<void> promise_{};
};