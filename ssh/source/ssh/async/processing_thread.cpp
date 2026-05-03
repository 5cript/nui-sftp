#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <stdexcept>
#include <future>

namespace SecureShell
{
    ProcessingThread::ProcessingThread()
    {}
    ProcessingThread::~ProcessingThread()
    {
        stop();
    }
    bool ProcessingThread::isRunning() const
    {
        return running_;
    }
    void ProcessingThread::start(std::chrono::milliseconds const& minimumCycleWait)
    {
        if (thread_.joinable())
            throw std::logic_error("ProcessingThread::start called while a previous thread is still joinable; call stop() first.");
        {
            std::lock_guard lock{taskMutex_};
            running_ = true;
        }
        shuttingDown_ = false;
        std::promise<void> awaitThreadStart{};
        thread_ = std::thread(
            [this, &awaitThreadStart, minimumCycleWait]
            {
                awaitThreadStart.set_value();
                processingThreadId_.store(std::this_thread::get_id());
                run(minimumCycleWait);
            }
        );
        awaitThreadStart.get_future().wait();
    }
    void ProcessingThread::stop()
    {
        shuttingDown_ = true;
        {
            std::lock_guard lock{taskMutex_};
            running_ = false;
        }
        if (thread_.joinable())
            thread_.join();

        // execute all pending tasks in FIFO order, matching the run loop:
        std::lock_guard lock{taskMutex_};
        while (!tasks_.empty())
        {
            tasks_.front()();
            tasks_.pop_front();
        }
        shuttingDown_ = false;
    }
    bool ProcessingThread::pushTask(std::function<void()> task)
    {
        if (!task)
        {
            throw std::invalid_argument("Task must not be empty.");
        }

        if (shuttingDown_)
        {
            return false;
        }

        {
            std::lock_guard lock{taskMutex_};
            tasks_.push_back(std::move(task));
        }
        return true;
    }
    std::unique_ptr<ProcessingStrand> ProcessingThread::createStrand()
    {
        return std::make_unique<ProcessingStrand>(this);
    }
    std::pair<bool, ProcessingThread::PermanentTaskId>
    ProcessingThread::pushPermanentTask(std::function<bool(ProcessingThread::PermanentTaskId id)> task)
    {
        if (!task)
        {
            throw std::invalid_argument("Task must not be empty.");
        }

        if (shuttingDown_)
        {
            return {false, PermanentTaskId{-1}};
        }

        PermanentTaskId id{-1};
        {
            std::lock_guard lock{taskMutex_};
            ++permanentTaskIdCounter_;
            id = PermanentTaskId{permanentTaskIdCounter_};
            permanentTasks_.insert({id, std::move(task)});
            permanentTasksAvailable_ = true;
        }
        return {true, id};
    }
    void ProcessingThread::clearPermanentTasks()
    {
        std::lock_guard lock{taskMutex_};
        if (processingPermanents_)
        {
            deferredTaskModification_.push_back(
                [this]()
                {
                    /* taskMutex_ is held here */
                    permanentTasks_.clear();
                    permanentTasksAvailable_ = false;
                }
            );
            return;
        }

        permanentTasks_.clear();
        permanentTasksAvailable_ = false;
    }
    bool ProcessingThread::removePermanentTask(PermanentTaskId const& id)
    {
        std::unique_lock lock{taskMutex_};
        if (processingPermanents_)
        {
            if (withinProcessingThread())
            {
                deferredTaskModification_.push_back(
                    [this, id]()
                    {
                        /* taskMutex_ is held here */
                        permanentTasks_.erase(id);
                        permanentTasksAvailable_ = !permanentTasks_.empty();
                    }
                );
                return true;
            }
            else
            {
                auto promise = std::make_shared<std::promise<bool>>();
                deferredTaskModification_.push_back(
                    [this, id, promise]()
                    {
                        /* taskMutex_ is held here */
                        const bool result = permanentTasks_.erase(id) > 0;
                        permanentTasksAvailable_ = !permanentTasks_.empty();
                        promise->set_value(result);
                    }
                );
                lock.unlock();
                return promise->get_future().wait_for(std::chrono::seconds{5}) == std::future_status::ready;
            }
        }

        auto result = permanentTasks_.erase(id);
        permanentTasksAvailable_ = !permanentTasks_.empty();
        return result > 0;
    }
    void ProcessingThread::run(std::chrono::milliseconds const& minimumCycleWait)
    {
        auto timePoint = std::chrono::steady_clock::now();

        try
        {
            std::vector<PermanentTaskId> toRemove{};
            std::exception_ptr pendingException{};
            while (running_)
            {
                timePoint = std::chrono::steady_clock::now();

                if (permanentTasksAvailable_)
                {
                    std::unique_lock lock(taskMutex_);
                    auto permaTasksMoved = std::move(permanentTasks_);
                    permanentTasks_ = {};
                    processingPermanents_ = true;
                    lock.unlock();

                    for (auto const& [id, task] : permaTasksMoved)
                    {
                        // Throwing tasks are removed (not re-run) and the exception is
                        // captured for rethrow at end of cycle so cleanup below still runs.
                        bool keep = false;
                        try
                        {
                            keep = task(id);
                        }
                        catch (...)
                        {
                            if (!pendingException)
                                pendingException = std::current_exception();
                        }
                        if (!keep)
                            toRemove.push_back(id);

                        if (!running_ || shuttingDown_)
                            break;
                    }
                    if (!toRemove.empty())
                    {
                        for (auto const& id : toRemove)
                            permaTasksMoved.erase(id);
                        toRemove.clear();
                    }

                    lock.lock();
                    processingPermanents_ = false;
                    if (permanentTasks_.empty())
                        permanentTasks_ = std::move(permaTasksMoved);
                    else
                    {
                        // Move em back the expensive way:
                        for (auto& [id, task] : permaTasksMoved)
                        {
                            permanentTasks_.insert({id, std::move(task)});
                        }
                    }
                    permanentTasksAvailable_ = !permanentTasks_.empty();
                    for (auto& modify : deferredTaskModification_)
                    {
                        modify();
                    }
                    deferredTaskModification_.clear();
                }

                {
                    std::vector<std::function<void()>> tasks{};
                    {
                        std::lock_guard lock(taskMutex_);
                        tasks.reserve(std::min(tasks_.size(), maximumTasksProcessableAtOnce));
                        std::move(
                            tasks_.begin(),
                            tasks_.begin() + std::min(tasks_.size(), maximumTasksProcessableAtOnce),
                            std::back_inserter(tasks)
                        );
                        tasks_.erase(
                            tasks_.begin(), tasks_.begin() + std::min(tasks_.size(), maximumTasksProcessableAtOnce)
                        );
                    }

                    for (auto const& task : tasks)
                    {
                        // Task is checked before adding, shouldnt possibly be empty:
#ifndef NDEBUG
                        if (!task)
                        {
                            throw std::runtime_error("Task must not be empty.");
                        }
#endif
                        try
                        {
                            task();
                        }
                        catch (...)
                        {
                            if (!pendingException)
                                pendingException = std::current_exception();
                        }
                    }
                }

                if (pendingException)
                    std::rethrow_exception(pendingException);

                if (minimumCycleWait.count() > 0)
                {
                    auto now = std::chrono::steady_clock::now();
                    auto diff = now - timePoint;
                    if (diff < minimumCycleWait)
                    {
                        std::this_thread::sleep_for(minimumCycleWait - diff);
                    }
                }
            }
        }
        catch (...)
        {
            std::lock_guard lock{taskMutex_};
            running_ = false;
        }
    }
    int ProcessingThread::permanentTaskCount() const
    {
        std::lock_guard lock{taskMutex_};
        return permanentTasks_.size();
    }
    bool ProcessingThread::awaitCycle(std::chrono::milliseconds maxWait)
    {
        bool running = false;
        {
            std::lock_guard lock{taskMutex_};
            running = running_;
        }
        if (!withinProcessingThread() && running)
        {
            return pushPromiseTask(
                       []()
                       {
                           return true;
                       }
                   ).wait_for(maxWait) == std::future_status::ready;
        }
        return false;
    }
}