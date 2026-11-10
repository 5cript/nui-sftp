#include <ftp/engine/task_gate.hpp>

#include <mutex>
#include <utility>

namespace FileTransferProtocol
{
    TaskGate::TaskGate(RemoteIo::ProcessingThread& thread)
        : thread_{&thread}
    {}

    bool TaskGate::push(std::function<void()> task)
    {
        std::shared_lock lock{mutex_};
        return thread_ != nullptr && thread_->pushTask(std::move(task));
    }

    bool TaskGate::onProcessingThread() const
    {
        std::shared_lock lock{mutex_};
        return thread_ != nullptr && thread_->withinProcessingThread();
    }

    void TaskGate::close()
    {
        std::unique_lock lock{mutex_};
        thread_ = nullptr;
    }
}
