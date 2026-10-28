#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <algorithm>
#include <stdexcept>
#include <future>

namespace SecureShell
{
    namespace
    {
        /**
         * @brief Makes the calling thread act as the processing thread until destroyed, for tasks run outside the loop.
         */
        class ActingProcessingThread
        {
          public:
            explicit ActingProcessingThread(std::atomic<std::thread::id>& processingThreadId)
                : processingThreadId_{processingThreadId}
                , previous_{processingThreadId.exchange(std::this_thread::get_id())}
            {}
            ~ActingProcessingThread()
            {
                processingThreadId_.store(previous_);
            }
            ActingProcessingThread(ActingProcessingThread const&) = delete;
            ActingProcessingThread& operator=(ActingProcessingThread const&) = delete;
            ActingProcessingThread(ActingProcessingThread&&) = delete;
            ActingProcessingThread& operator=(ActingProcessingThread&&) = delete;

          private:
            std::atomic<std::thread::id>& processingThreadId_;
            std::thread::id previous_;
        };
    }

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
            std::lock_guard cleanupLock{cleanupMutex_};
            std::lock_guard lock{taskMutex_};
            running_ = true;
            intake_ = Intake::Open;
        }
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
        bool interruptSocketWait = false;
        {
            // Waits for cleanup tasks that run on a caller, the drain must not run alongside them.
            std::lock_guard cleanupLock{cleanupMutex_};
            std::lock_guard lock{taskMutex_};
            intake_ = Intake::Draining;
            running_ = false;
            wakeRequested_ = true;
            interruptSocketWait = waitingOnSocket_;
        }
        notifyWake(interruptSocketWait);
        if (thread_.joinable())
            thread_.join();
        // The id of a joined thread may be reused.
        processingThreadId_.store({});

        // execute all pending tasks in FIFO order, matching the run loop, and outside the lock so pushes meanwhile are
        // refused instead of waiting for the drain:
        const ActingProcessingThread acting{processingThreadId_};
        std::unique_lock lock{taskMutex_};
        while (!tasks_.empty())
        {
            auto task = std::move(tasks_.front().run);
            tasks_.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }
        intake_ = Intake::Closed;
    }
    bool ProcessingThread::pushTask(std::function<void()> task)
    {
        if (!task)
        {
            throw std::invalid_argument("Task must not be empty.");
        }

        bool interruptSocketWait = false;
        {
            std::lock_guard lock{taskMutex_};
            if (intake_ != Intake::Open)
            {
                return false;
            }
            tasks_.push_back({.run = std::move(task)});
            wakeRequested_ = true;
            interruptSocketWait = waitingOnSocket_;
        }
        notifyWake(interruptSocketWait);
        return true;
    }
    void ProcessingThread::pushCleanupTask(std::function<void()> task)
    {
        if (!task)
        {
            throw std::invalid_argument("Task must not be empty.");
        }

        // Held while the task runs here, so cleanup tasks run one at a time and never alongside a restarted thread.
        std::lock_guard cleanupLock{cleanupMutex_};
        bool queued = false;
        bool interruptSocketWait = false;
        {
            std::lock_guard lock{taskMutex_};
            if (intake_ != Intake::Closed)
            {
                tasks_.push_back({.run = std::move(task), .cleanup = true});
                wakeRequested_ = true;
                interruptSocketWait = waitingOnSocket_;
                queued = true;
            }
        }
        if (queued)
        {
            notifyWake(interruptSocketWait);
            return;
        }
        const ActingProcessingThread acting{processingThreadId_};
        task();
    }
    void ProcessingThread::wake()
    {
        bool interruptSocketWait = false;
        {
            std::lock_guard lock{taskMutex_};
            wakeRequested_ = true;
            interruptSocketWait = waitingOnSocket_;
        }
        notifyWake(interruptSocketWait);
    }
    void ProcessingThread::notifyWake(bool interruptSocketWait)
    {
        wakeUp_.notify_all();
        // Set before waitingOnSocket_ under the same lock, and destroyed only after the thread ended.
        if (interruptSocketWait)
            socketWait_->wake();
    }
    std::expected<void, std::string> ProcessingThread::watchSocket(std::function<SocketWait::Watched()> watched)
    {
        std::lock_guard lock{taskMutex_};
        if (!socketWait_)
        {
            auto created = SocketWait::create();
            if (!created)
                return std::unexpected(std::move(created).error());
            socketWait_ = std::move(created).value();
        }
        watchedSocket_ = std::move(watched);
        return {};
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

        PermanentTaskId id{-1};
        bool interruptSocketWait = false;
        {
            std::lock_guard lock{taskMutex_};
            if (intake_ != Intake::Open)
            {
                return {false, PermanentTaskId{-1}};
            }
            ++permanentTaskIdCounter_;
            id = PermanentTaskId{permanentTaskIdCounter_};
            permanentTasks_.insert({id, PermanentTask{.run = std::move(task), .pollInterval = pollInterval}});
            permanentTasksAvailable_ = true;
            wakeRequested_ = true;
            interruptSocketWait = waitingOnSocket_;
        }
        notifyWake(interruptSocketWait);
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
                auto removed = promise->get_future();
                return removed.wait_for(std::chrono::seconds{5}) == std::future_status::ready && removed.get();
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
                CycleNeeds needs{};

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
                        const auto lookAgain = cycleStart_ + task.pollInterval.value_or(pollInterval);
                        if (status == PermanentTaskStatus::Done)
                            toRemove.push_back(id);
                        else
                            needs.mostUrgent = std::min(needs.mostUrgent, status);
                        if (status == PermanentTaskStatus::Poll)
                            needs.nextPoll = std::min(needs.nextPoll, lookAgain);
                        if (status == PermanentTaskStatus::AwaitInput)
                        {
                            needs.awaitsInput = true;
                            needs.inputFallback = std::min(needs.inputFallback, lookAgain);
                        }
                        return running_.load();
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
                    std::vector<QueuedTask> tasks{};
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
                        if (!task.run)
                        {
                            throw std::runtime_error("Task must not be empty.");
                        }
#endif
                        try
                        {
                            task.run();
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

                waitForWork(needs);
            }
        }
        catch (...)
        {
            // Cleanup tasks pushed from now on run on their caller, after the queued ones ran here.
            std::lock_guard cleanupLock{cleanupMutex_};
            std::deque<QueuedTask> pending{};
            {
                std::lock_guard lock{taskMutex_};
                running_ = false;
                intake_ = Intake::Closed;
                pending = std::exchange(tasks_, {});
            }
            // Queued tasks would run only when stop() drains, which the threads waiting for them may never reach.
            // Dropped promise tasks complete with their unfinished result, cleanup tasks still run.
            for (auto& task : pending)
            {
                if (!task.cleanup)
                    continue;
                try
                {
                    task.run();
                }
                catch (...)
                {
                    // The loop already ended on an exception.
                }
            }
        }
    }
    void ProcessingThread::waitForWork(CycleNeeds const& needs)
    {
        if (needs.mostUrgent == PermanentTaskStatus::Busy)
            return;

        std::unique_lock lock{taskMutex_};
        const auto workArrived = [this]()
        {
            return !running_ || wakeRequested_ || !tasks_.empty();
        };
        if (workArrived())
            return;

        std::optional<std::chrono::steady_clock::time_point> deadline{};
        if (needs.mostUrgent == PermanentTaskStatus::Poll)
            deadline = needs.nextPoll;
        if (needs.awaitsInput)
        {
            const auto watched = socketWait_ && watchedSocket_ ? watchedSocket_() : SocketWait::Watched{};
            if (watched.socket != SSH_INVALID_SOCKET)
            {
                waitingOnSocket_ = true;
                lock.unlock();
                const auto result = socketWait_->wait(watched, deadline);
                lock.lock();
                waitingOnSocket_ = false;
                // A failing poll must not spin, so the tasks look again after their interval.
                if (result == SocketWait::Result::Failed)
                    wakeUp_.wait_until(lock, needs.inputFallback, workArrived);
                return;
            }
            deadline = std::min(deadline.value_or(needs.inputFallback), needs.inputFallback);
        }
        if (deadline)
            wakeUp_.wait_until(lock, *deadline, workArrived);
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
            auto cycled = pushPromiseTask(
                []()
                {
                    return true;
                }
            );
            return cycled.wait_for(maxWait) == std::future_status::ready && cycled.get();
        }
        return false;
    }
}