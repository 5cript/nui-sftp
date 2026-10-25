#pragma once

#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <functional>
#include <map>
#include <optional>
#include <future>
#include <memory>
#include <vector>

namespace SecureShell
{
    class ProcessingStrand;

    /**
     * @brief A processing thread that can be used to execute tasks sequentially in a separate thread.
     * This allows for complex simultaneous operations to be executed in a single thread.
     */
    class ProcessingThread
    {
      public:
        constexpr static std::size_t maximumTasksProcessableAtOnce = 100ull;

        /**
         * @brief A permanent task id that can be used to remove permanent tasks.
         */
        struct PermanentTaskId
        {
            int id = -1;
            friend auto operator<=>(PermanentTaskId const& lhs, PermanentTaskId const& rhs) noexcept
            {
                return lhs.id <=> rhs.id;
            }
        };

        /**
         * @brief What a permanent task needs after its turn, which decides whether the thread may sleep.
         */
        enum class PermanentTaskStatus
        {
            /** @brief The task is finished and is removed. */
            Done,
            /** @brief The task has more work, the next cycle starts right away. */
            Busy,
            /** @brief Nothing to do now, but the task must look again after the poll interval (a channel reader). */
            Poll,
            /** @brief Nothing to do until someone calls wake (a paused transfer). */
            Idle,
        };

        ProcessingThread();
        ~ProcessingThread();
        ProcessingThread(ProcessingThread const&) = delete;
        ProcessingThread& operator=(ProcessingThread const&) = delete;
        ProcessingThread(ProcessingThread&&) = delete;
        ProcessingThread& operator=(ProcessingThread&&) = delete;

        /**
         * @brief Starts the processing thread.
         *
         * Between cycles the thread sleeps until a task is pushed or wake is called, and no longer than the poll
         * interval while a permanent task polls. A busy permanent task keeps it from sleeping.
         *
         * @param pollInterval How long the thread sleeps at most while a permanent task polls.
         */
        void start(std::chrono::milliseconds const& pollInterval = std::chrono::milliseconds{0});

        /**
         * @brief Wakes the thread for a cycle, for a permanent task whose state changed from outside.
         */
        void wake();

        /**
         * @brief Stops the processing thread. Executes all pending tasks.
         */
        void stop();

        /**
         * @brief Returns true if the processing thread is running.
         *
         * @return true If the processing thread is running.
         * @return false If the processing thread is not running.
         */
        bool isRunning() const;

        /**
         * @brief Pushes a task to the processing thread.
         *
         * @param task The task to push.
         * @return true If the task was pushed.
         * @return false If the task was empty or the processing thread is shutting down.
         */
        bool pushTask(std::function<void()> task);

        /**
         * @brief Pushes a task that has a return value to the processing thread.
         * The return value is then accessible through the returned future.
         *
         * @param func The function to execute.
         * @return std::future<std::invoke_result_t<std::decay_t<Func>>> The future that will contain the return value.
         */
        template <typename Func>
        auto pushPromiseTask(Func&& func) -> std::future<std::invoke_result_t<std::decay_t<Func>>>
        {
            using ReturnType = std::invoke_result_t<std::decay_t<Func>>;
            auto promise = std::make_shared<std::promise<ReturnType>>();
            pushTask(
                [promise, func = std::forward<Func>(func)]() mutable
                {
                    if constexpr (std::is_void_v<ReturnType>)
                    {
                        func();
                        promise->set_value();
                    }
                    else
                    {
                        promise->set_value(func());
                    }
                }
            );
            return promise->get_future();
        }

        /**
         * @brief Pushes a task that is not removed upon execution. Will be executed every cycle.
         *
         * @param task The task to push.
         * @return std::pair<bool, PermanentTaskId> If the task was pushed and the id of the task.
         */
        std::pair<bool, PermanentTaskId> pushPermanentTask(
            std::function<PermanentTaskStatus(ProcessingThread::PermanentTaskId)> task,
            std::optional<std::chrono::milliseconds> pollInterval = std::nullopt
        );

        /**
         * @brief Pushes a permanent task that polls: true keeps it, false removes it.
         *
         * @param pollInterval How often the task looks at least, the thread's poll interval if unset.
         */
        std::pair<bool, PermanentTaskId> pushPermanentTask(
            std::function<bool(ProcessingThread::PermanentTaskId)> task,
            std::optional<std::chrono::milliseconds> pollInterval = std::nullopt
        );

        /**
         * @brief Removes a permanent task.
         *
         * @param id The id of the task to remove.
         * @return true If the task was removed.
         * @return false If the task was not found.
         */
        bool removePermanentTask(PermanentTaskId const& id);

        /**
         * @brief Returns the number of permanent tasks.
         *
         * @return int The number of permanent tasks.
         */
        int permanentTaskCount() const;

        /**
         * @brief Time spent in the current processing cycle. Permanent tasks that block (file
         *        transfers) use it to skip their turn once the cycle has run long, which keeps
         *        one-shot tasks responsive. Only meaningful from within the processing thread.
         */
        std::chrono::steady_clock::duration cycleElapsed() const noexcept;

        /**
         * @brief Removes all permanent tasks.
         */
        void clearPermanentTasks();

        /**
         * @brief Waits for a cycle to complete.
         *
         * @param maxWait The maximum time to wait for a cycle.
         * @return true If a cycle was completed.
         * @return false If the maximum wait time was reached.
         */
        bool awaitCycle(std::chrono::milliseconds maxWait = std::chrono::seconds{5});

        /**
         * @brief Returns true if the current thread is the processing thread.
         *
         * @return true If the current thread is the processing thread.
         * @return false If the current thread is not the processing thread.
         */
        bool withinProcessingThread() const noexcept
        {
            return processingThreadId_ == std::this_thread::get_id();
        }

        /**
         * @brief Creates a new processing strand.
         *
         * @return std::unique_ptr<ProcessingStrand> The processing strand.
         */
        std::unique_ptr<ProcessingStrand> createStrand();

      private:
        void run(std::chrono::milliseconds const& pollInterval);

        /**
         * @brief Sleeps after a cycle as far as its permanent tasks allow, until something wakes the thread.
         *
         * @param nextPoll When the earliest polling task wants to look again.
         */
        void waitForWork(PermanentTaskStatus mostUrgent, std::chrono::steady_clock::time_point nextPoll);

        /**
         * @brief A permanent task and how often it polls.
         */
        struct PermanentTask
        {
            std::function<PermanentTaskStatus(PermanentTaskId)> run;
            std::optional<std::chrono::milliseconds> pollInterval;
        };

      private:
        std::thread thread_{};
        mutable std::recursive_mutex taskMutex_{};
        std::atomic<bool> running_ = false;
        std::atomic<bool> shuttingDown_ = false;
        std::atomic_bool permanentTasksAvailable_ = false;
        std::atomic<int> permanentTaskIdCounter_ = 0;
        std::atomic<std::thread::id> processingThreadId_{};
        std::atomic_bool processingPermanents_{false};
        // Written by the processing thread at the start of each cycle, read by tasks on it.
        std::chrono::steady_clock::time_point cycleStart_{};
        // Permanent tasks start at a different position each cycle so tasks late in the map
        // are not starved when earlier ones exhaust the cycle.
        std::size_t permanentRotation_{0};
        std::vector<std::function<void()>> deferredTaskModification_{};
        std::condition_variable_any wakeUp_{};
        /** @brief Set under taskMutex_ by everything that needs a cycle, cleared when a cycle starts. */
        bool wakeRequested_{false};

        std::deque<std::function<void()>> tasks_{};
        std::map<PermanentTaskId, PermanentTask> permanentTasks_{};
    };
}