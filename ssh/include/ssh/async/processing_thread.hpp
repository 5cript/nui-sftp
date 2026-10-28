#pragma once

#include <ssh/async/socket_wait.hpp>
#include <ssh/async/unfinished_task_result.hpp>

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
#include <type_traits>
#include <utility>
#include <vector>

namespace SecureShell
{
    class ProcessingStrand;

    namespace Detail
    {
        /**
         * @brief The promise of a promise task, shared by the copies of the task. When the last copy is gone and the
         *        task did not finish, the promise gets the unfinished result.
         */
        template <typename ResultT, typename UnfinishedT>
        class PromiseTaskState
        {
          public:
            explicit PromiseTaskState(UnfinishedT unfinished)
                : unfinished_{std::move(unfinished)}
            {}
            ~PromiseTaskState()
            {
                if (!finished_)
                    complete(promise_, unfinished_);
            }
            PromiseTaskState(PromiseTaskState const&) = delete;
            PromiseTaskState& operator=(PromiseTaskState const&) = delete;
            PromiseTaskState(PromiseTaskState&&) = delete;
            PromiseTaskState& operator=(PromiseTaskState&&) = delete;

            std::future<ResultT> future()
            {
                return promise_.get_future();
            }

            template <typename FunctionT>
            void run(FunctionT& function)
            {
                complete(promise_, function);
                finished_ = true;
            }

            /**
             * @brief Sets @p promise to what @p produce returns.
             */
            template <typename FunctionT>
            static void complete(std::promise<ResultT>& promise, FunctionT& produce)
            {
                if constexpr (std::is_void_v<ResultT>)
                {
                    produce();
                    promise.set_value();
                }
                else
                {
                    promise.set_value(produce());
                }
            }

          private:
            std::promise<ResultT> promise_{};
            UnfinishedT unfinished_;
            bool finished_{false};
        };

        /**
         * @brief Makes a task of @p func that completes the promise of @p state.
         */
        template <typename StateT, typename FunctionT>
        std::function<void()> makePromiseTask(std::shared_ptr<StateT> state, FunctionT&& func)
        {
            return [state = std::move(state), func = std::forward<FunctionT>(func)]() mutable
            {
                state->run(func);
            };
        }

        /**
         * @brief A future that already holds the unfinished result, for a task refused before it reached a thread.
         */
        template <typename ResultT, typename UnfinishedT>
        std::future<ResultT> unfinishedFuture(UnfinishedT unfinished)
        {
            std::promise<ResultT> promise{};
            PromiseTaskState<ResultT, UnfinishedT>::complete(promise, unfinished);
            return promise.get_future();
        }
    }

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
            /** @brief Nothing to do now, but the task must look again after the poll interval. */
            Poll,
            /**
             * @brief Nothing to do until the watched socket has data or someone calls wake (a channel reader). Without
             *        a watched socket this polls.
             */
            AwaitInput,
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
         * interval while a permanent task polls. While one awaits input it also wakes when the watched socket has
         * data. A busy permanent task keeps it from sleeping.
         *
         * @param pollInterval How long the thread sleeps at most while a permanent task polls.
         */
        void start(std::chrono::milliseconds const& pollInterval = std::chrono::milliseconds{0});

        /**
         * @brief Wakes the thread for a cycle, for a permanent task whose state changed from outside.
         */
        void wake();

        /**
         * @brief Sets the socket the thread waits on while a permanent task awaits input.
         *
         * @param watched Called on the processing thread before every such wait, so it may ask libssh for the
         *        socket, which libssh closes on a fatal error. An invalid socket falls back to polling.
         * @return Why the wake handle could not be created, the thread then keeps polling.
         */
        std::expected<void, std::string> watchSocket(std::function<SocketWait::Watched()> watched);

        /**
         * @brief Stops the processing thread. Executes all pending tasks, on this thread acting as the processing
         *        thread. Refuses pushes from now until the next start.
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
         * @return false If the processing thread was stopped and not started again, or its loop ended on an exception.
         */
        bool pushTask(std::function<void()> task);

        /**
         * @brief Pushes a task that must run, a strand's final task. It is queued like pushTask while the loop or the
         *        drain of stop() will run it, and still runs when the loop ends on an exception. Once neither will, it
         *        runs on the calling thread at once, which acts as the processing thread meanwhile.
         *
         * @param task The task to push.
         */
        void pushCleanupTask(std::function<void()> task);

        /**
         * @brief Pushes a task that has a return value to the processing thread.
         * The return value is then accessible through the returned future.
         *
         * @param func The function to execute.
         * @param unfinished Makes the future's value when the task does not finish, see UnfinishedTaskResult.
         * @return std::future<std::invoke_result_t<std::decay_t<Func>>> The future that will contain the return value.
         */
        template <
            typename Func,
            typename UnfinishedFunc = UnfinishedTaskResult<std::invoke_result_t<std::decay_t<Func>>>>
        auto pushPromiseTask(Func&& func, UnfinishedFunc unfinished = {})
            -> std::future<std::invoke_result_t<std::decay_t<Func>>>
        {
            using ReturnType = std::invoke_result_t<std::decay_t<Func>>;
            auto state = std::make_shared<Detail::PromiseTaskState<ReturnType, UnfinishedFunc>>(std::move(unfinished));
            auto future = state->future();
            pushTask(Detail::makePromiseTask(std::move(state), std::forward<Func>(func)));
            return future;
        }

        /**
         * @brief Works like pushCleanupTask but returns a future for the return value of the task.
         *
         * @param func The function to execute.
         * @param unfinished Makes the future's value when the task throws, see UnfinishedTaskResult.
         */
        template <
            typename Func,
            typename UnfinishedFunc = UnfinishedTaskResult<std::invoke_result_t<std::decay_t<Func>>>>
        auto pushCleanupPromiseTask(Func&& func, UnfinishedFunc unfinished = {})
            -> std::future<std::invoke_result_t<std::decay_t<Func>>>
        {
            using ReturnType = std::invoke_result_t<std::decay_t<Func>>;
            auto state = std::make_shared<Detail::PromiseTaskState<ReturnType, UnfinishedFunc>>(std::move(unfinished));
            auto future = state->future();
            pushCleanupTask(Detail::makePromiseTask(std::move(state), std::forward<Func>(func)));
            return future;
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
         * @brief What the permanent tasks of one cycle need from the wait after it.
         */
        struct CycleNeeds
        {
            PermanentTaskStatus mostUrgent{PermanentTaskStatus::Idle};
            /** @brief When the earliest polling task wants to look again. */
            std::chrono::steady_clock::time_point nextPoll{std::chrono::steady_clock::time_point::max()};
            bool awaitsInput{false};
            /** @brief When tasks that await input look again if there is no socket to wait on. */
            std::chrono::steady_clock::time_point inputFallback{std::chrono::steady_clock::time_point::max()};
        };

        /**
         * @brief Sleeps after a cycle as far as its permanent tasks allow, until something wakes the thread.
         */
        void waitForWork(CycleNeeds const& needs);

        /**
         * @brief Wakes the thread after wakeRequested_ was set.
         *
         * @param interruptSocketWait Whether the thread was waiting on the socket when the flag was set.
         */
        void notifyWake(bool interruptSocketWait);

        /**
         * @brief A permanent task and how often it polls.
         */
        struct PermanentTask
        {
            std::function<PermanentTaskStatus(PermanentTaskId)> run;
            std::optional<std::chrono::milliseconds> pollInterval;
        };

        /**
         * @brief A queued task. Only cleanup tasks still run when the loop ends on an exception.
         */
        struct QueuedTask
        {
            std::function<void()> run;
            bool cleanup{false};
        };

        /**
         * @brief What happens to pushed tasks.
         */
        enum class Intake
        {
            /** @brief Tasks are queued: the thread was constructed or started. */
            Open,
            /** @brief stop() is running: tasks are refused, cleanup tasks are queued for its drain. */
            Draining,
            /** @brief Stopped, or the loop threw: tasks are refused, cleanup tasks run on the caller. */
            Closed,
        };

      private:
        std::thread thread_{};
        /** @brief Held while cleanup tasks run outside the loop and while intake_ changes, taken before taskMutex_. */
        std::recursive_mutex cleanupMutex_{};
        mutable std::recursive_mutex taskMutex_{};
        std::atomic<bool> running_ = false;
        /** @brief Under taskMutex_. */
        Intake intake_{Intake::Open};
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
        /** @brief Under taskMutex_: the thread sleeps in socketWait_, so a wake must go through it. */
        bool waitingOnSocket_{false};
        std::function<SocketWait::Watched()> watchedSocket_{};
        std::unique_ptr<SocketWait> socketWait_{};

        std::deque<QueuedTask> tasks_{};
        std::map<PermanentTaskId, PermanentTask> permanentTasks_{};
    };
}