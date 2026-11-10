#include <ftp/engine/work_counter.hpp>

namespace FileTransferProtocol
{
    WorkCounter::Token WorkCounter::acquire()
    {
        {
            std::scoped_lock lock{mutex_};
            ++count_;
        }
        return Token{
            static_cast<void*>(this),
            [self = shared_from_this()](void*)
            {
                self->releaseOne();
            }
        };
    }

    bool WorkCounter::waitIdle(std::chrono::milliseconds limit)
    {
        std::unique_lock lock{mutex_};
        return idle_.wait_for(
            lock,
            limit,
            [this]()
            {
                return count_ == 0u;
            }
        );
    }

    std::size_t WorkCounter::count() const
    {
        std::scoped_lock lock{mutex_};
        return count_;
    }

    void WorkCounter::releaseOne()
    {
        std::scoped_lock lock{mutex_};
        if (--count_ == 0u)
            idle_.notify_all();
    }
}
