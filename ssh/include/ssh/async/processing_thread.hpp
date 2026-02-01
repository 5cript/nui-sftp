#pragma once

#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>
#include <map>
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

        ProcessingThread();
        ~ProcessingThread();
        ProcessingThread(ProcessingThread const&) = delete;
        ProcessingThread& operator=(ProcessingThread const&) = delete;
        ProcessingThread(ProcessingThread&&) = delete;
        ProcessingThread& operator=(ProcessingThread&&) = delete;

        /**
         * @brief Starts the processing thread.
         *
         * @param waitCycleTimeout The time to wait for a task to become available before checking if the thread should
         * stop.
         * @param minimumCycleWait The minimum wait time between cycles to throttle the processing thread in case of the
         * existence of permanent tasks.
         */
        void start(std::chrono::milliseconds const& minimumCycleWait = std::chrono::milliseconds{0});

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
        std::pair<bool, PermanentTaskId> pushPermanentTask(std::function<bool(ProcessingThread::PermanentTaskId)> task);

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
        void run(std::chrono::milliseconds const& minimumCycleWait);

      private:
        std::thread thread_{};
        mutable std::recursive_mutex taskMutex_{};
        std::atomic<bool> running_ = false;
        std::atomic<bool> shuttingDown_ = false;
        std::atomic_bool permanentTasksAvailable_ = false;
        std::atomic<int> permanentTaskIdCounter_ = 0;
        std::atomic<std::thread::id> processingThreadId_{};
        std::atomic_bool processingPermanents_{false};
        std::vector<std::function<void()>> deferredTaskModification_{};

        std::deque<std::function<void()>> tasks_{};
        std::map<PermanentTaskId, std::function<bool(ProcessingThread::PermanentTaskId)>> permanentTasks_{};
    };
}