#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <algorithm>
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
    void ProcessingThread::start(std::chrono::milliseconds const& pollInterval)
    {
        if (thread_.joinable())
            throw std::logic_error(
                "ProcessingThread::start called while a previous thread is still joinable; call stop() first."
            );
        {
            std::lock_guard lock{taskMutex_};
            running_ = true;
        }
        shuttingDown_ = false;
        std::promise<void> awaitThreadStart{};
        thread_ = std::thread(
            [this, &awaitThreadStart, pollInterval]
            {
                awaitThreadStart.set_value();
                processingThreadId_.store(std::this_thread::get_id());
                run(pollInterval);
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
            wakeRequested_ = true;
        }
        wakeUp_.notify_all();
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
            wakeRequested_ = true;
        }
        wakeUp_.notify_one();
        return true;
    }
    void ProcessingThread::wake()
    {
        {
            std::lock_guard lock{taskMutex_};
            wakeRequested_ = true;
        }
        wakeUp_.notify_one();
    }
    std::unique_ptr<ProcessingStrand> ProcessingThread::createStrand()
    {
        return std::make_unique<ProcessingStrand>(this);
    }
    std::pair<bool, ProcessingThread::PermanentTaskId> ProcessingThread::pushPermanentTask(
        std::function<bool(ProcessingThread::PermanentTaskId id)> task,
        std::optional<std::chrono::milliseconds> pollInterval
    )
    {
        if (!task)
        {
            throw std::invalid_argument("Task must not be empty.");
        }
        return pushPermanentTask(
            std::function<PermanentTaskStatus(PermanentTaskId)>{
                [task = std::move(task)](PermanentTaskId id)
                {
                    return task(id) ? PermanentTaskStatus::Poll : PermanentTaskStatus::Done;
                }
            },
            pollInterval
        );
    }
    std::pair<bool, ProcessingThread::PermanentTaskId> ProcessingThread::pushPermanentTask(
        std::function<PermanentTaskStatus(ProcessingThread::PermanentTaskId id)> task,
        std::optional<std::chrono::milliseconds> pollInterval
    )
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
            permanentTasks_.insert({id, PermanentTask{.run = std::move(task), .pollInterval = pollInterval}});
            permanentTasksAvailable_ = true;
            wakeRequested_ = true;
        }
        wakeUp_.notify_one();
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
    void ProcessingThread::run(std::chrono::milliseconds const& pollInterval)
    {
        try
        {
            std::vector<PermanentTaskId> toRemove{};
            std::exception_ptr pendingException{};
            while (running_)
            {
                {
                    std::lock_guard lock{taskMutex_};
                    wakeRequested_ = false;
                }
                cycleStart_ = std::chrono::steady_clock::now();
                auto mostUrgent = PermanentTaskStatus::Idle;
                auto nextPoll = std::chrono::steady_clock::time_point::max();

                if (permanentTasksAvailable_)
                {
                    std::unique_lock lock(taskMutex_);
                    auto permaTasksMoved = std::move(permanentTasks_);
                    permanentTasks_ = {};
                    processingPermanents_ = true;
                    lock.unlock();

                    const auto taskCount = permaTasksMoved.size();
                    const auto rotation = taskCount == 0 ? 0 : (permanentRotation_ % taskCount);
                    const auto rotatedBegin = std::next(permaTasksMoved.begin(), static_cast<std::ptrdiff_t>(rotation));
                    std::size_t position = 0;
                    std::size_t longestPosition = 0;
                    std::chrono::steady_clock::duration longest{0};
                    auto runTask = [&](auto const& entry)
                    {
                        auto const& [id, task] = entry;
                        // Throwing tasks are removed (not re-run) and the exception is
                        // captured for rethrow at end of cycle so cleanup below still runs.
                        auto status = PermanentTaskStatus::Done;
                        const auto taskStart = std::chrono::steady_clock::now();
                        try
                        {
                            status = task.run(id);
                        }
                        catch (...)
                        {
                            if (!pendingException)
                                pendingException = std::current_exception();
                        }
                        const auto taskTook = std::chrono::steady_clock::now() - taskStart;
                        if (taskTook > longest)
                        {
                            longest = taskTook;
                            longestPosition = position;
                        }
                        ++position;
                        if (status == PermanentTaskStatus::Done)
                            toRemove.push_back(id);
                        else
                            mostUrgent = std::min(mostUrgent, status);
                        if (status == PermanentTaskStatus::Poll)
                            nextPoll = std::min(nextPoll, cycleStart_ + task.pollInterval.value_or(pollInterval));
                        return running_ && !shuttingDown_;
                    };
                    bool keepGoing = true;
                    for (auto task = rotatedBegin; keepGoing && task != permaTasksMoved.end(); ++task)
                        keepGoing = runTask(*task);
                    for (auto task = permaTasksMoved.begin(); keepGoing && task != rotatedBegin; ++task)
                        keepGoing = runTask(*task);
                    // Start the next cycle after the task that spent this one, so tasks that block
                    // take turns no matter how many cheap tasks sit between them in the map.
                    // Without a blocking task, advance one slot.
                    constexpr auto blockingTask = std::chrono::milliseconds{1};
                    if (taskCount != 0)
                        permanentRotation_ = (rotation + (longest >= blockingTask ? longestPosition + 1 : 1)) % taskCount;
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

                waitForWork(mostUrgent, nextPoll);
            }
        }
        catch (...)
        {
            std::lock_guard lock{taskMutex_};
            running_ = false;
        }
    }
    void ProcessingThread::waitForWork(PermanentTaskStatus mostUrgent, std::chrono::steady_clock::time_point nextPoll)
    {
        if (mostUrgent == PermanentTaskStatus::Busy)
            return;

        std::unique_lock lock{taskMutex_};
        const auto workArrived = [this]()
        {
            return !running_ || wakeRequested_ || !tasks_.empty();
        };
        if (mostUrgent == PermanentTaskStatus::Poll)
            wakeUp_.wait_until(lock, nextPoll, workArrived);
        else
            wakeUp_.wait(lock, workArrived);
    }
    std::chrono::steady_clock::duration ProcessingThread::cycleElapsed() const noexcept
    {
        return std::chrono::steady_clock::now() - cycleStart_;
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