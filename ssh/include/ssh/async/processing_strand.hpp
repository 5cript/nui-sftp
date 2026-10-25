#pragma once

#include <ssh/async/processing_thread.hpp>

#include <atomic>
#include <future>
#include <functional>
#include <mutex>
#include <set>

namespace SecureShell
{
    /**
     * @brief This class makes it impossible to push tasks to a strand after it has been finalized.
     * Finalization usually means some close operation is happening and no more tasks should be pushed.
     * Like reading a file when the session is closed.
     */
    class ProcessingStrand
    {
      public:
        /**
         * @brief Construct a new Processing Strand object living on top of a processing thread.
         *
         * @param processingThread The processing thread to push tasks to.
         */
        ProcessingStrand(ProcessingThread* processingThread)
            : processingThread_(processingThread)
        {}

        /**
         * @brief Pushes a task, but not if the strand has been finalized and not simultaneously with any other push.
         *
         * @param task The task to push.
         * @return true If the task was pushed.
         * @return false If the strand has been finalized or the task was empty.
         */
        bool pushTask(std::function<void()> task)
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
                return false;
            return processingThread_->pushTask(std::move(task));
        }

        /**
         * @brief Pushes a task that will be executed until the strand is finalized.
         * But not if the strand has been finalized and not simultaneously with any other push.
         *
         * @param task The task to push.
         * @return std::pair<bool, ProcessingThread::PermanentTaskId> If the task was pushed and the id of the task.
         */
        std::pair<bool, ProcessingThread::PermanentTaskId> pushPermanentTask(std::function<bool()> task)
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
                return {false, ProcessingThread::PermanentTaskId{-1}};
            // The wrapper does NOT erase the id from permanentTasks_ on self-removal: doing so would
            // require holding mutex_ from the processing thread, which can deadlock against
            // pushFinalPromiseTask (which holds mutex_ while waiting on the processing thread).
            // Stale ids are reaped at finalization, where ProcessingThread::removePermanentTask
            // is a harmless no-op for unknown ids.
            auto result = processingThread_->pushPermanentTask(
                [task = std::move(task)](ProcessingThread::PermanentTaskId)
                {
                    return task();
                }
            );
            this->permanentTasks_.insert(result.second);
            return result;
        }

        /**
         * @brief Removes a permanent task.
         *
         * @param id The id of the task to remove.
         */
        void removePermanentTask(ProcessingThread::PermanentTaskId const& id)
        {
            std::scoped_lock lock(mutex_);
            if (permanentTasks_.erase(id) > 0)
                processingThread_->removePermanentTask(id);
        }

        /**
         * @brief Pushes a task that makes any further pushes impossible. Also removes all permanent tasks.
         *
         * @param task The task to push.
         */
        void pushFinalTask(std::function<void()> task)
        {
            auto* processingThread = finalize();
            if (processingThread != nullptr)
                processingThread->pushTask(std::move(task));
        }

        /**
         * @brief Works like pushFinalTask but returns a future for the return value of the task.
         *
         * @param task The task to run.
         */
        template <typename FunctionT>
        auto pushFinalPromiseTask(FunctionT&& func) -> std::future<std::invoke_result_t<std::decay_t<FunctionT>>>
        {
            auto* processingThread = finalize();
            if (processingThread == nullptr)
            {
                std::promise<std::invoke_result_t<std::decay_t<FunctionT>>> promise{};
                promise.set_exception(
                    std::make_exception_ptr(std::runtime_error("Cannot push task to finalized strand."))
                );
                return promise.get_future();
            }
            return processingThread->pushPromiseTask(std::forward<FunctionT>(func));
        }

        /**
         * @brief Does a final task but runs it on the current calling thread.
         * Otherwise behaves like pushFinalTask.
         *
         * @param task The task to run.
         */
        void doFinalSync(std::function<void()> const& task)
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
                return;
            finalized_ = true;
            for (auto const& id : permanentTasks_)
            {
                processingThread_->removePermanentTask(id);
            }
            task();
        }

        /**
         * @brief Pushes a task the return of which is returned for a future.
         *
         * @tparam Func The type of the task.
         * @param func The task to push.
         * @return std::future<std::invoke_result_t<std::decay_t<Func>>> The future of the return value of the task.
         */
        template <typename Func>
        auto pushPromiseTask(Func&& func) -> std::future<std::invoke_result_t<std::decay_t<Func>>>
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
            {
                std::promise<std::invoke_result_t<std::decay_t<Func>>> promise{};
                promise.set_exception(
                    std::make_exception_ptr(std::runtime_error("Cannot push task to finalized strand."))
                );
                return promise.get_future();
            }
            return processingThread_->pushPromiseTask(std::forward<Func>(func));
        }

        /**
         * @brief Time spent in the processing thread's current cycle. See ProcessingThread::cycleElapsed.
         */
        std::chrono::steady_clock::duration cycleElapsed() const noexcept
        {
            return processingThread_->cycleElapsed();
        }

        bool withinProcessingThread() const noexcept
        {
            return processingThread_->withinProcessingThread();
        }

        bool isFinalized() const noexcept
        {
            return finalized_;
        }

      private:
        /**
         * @brief Marks the strand finalized and removes its permanent tasks.
         *
         * The final task is pushed after the lock is released: it usually destroys the owner of
         * this strand, and with it the mutex, while the pushing thread would still unlock it.
         *
         * @return The thread to push the final task to, null if the strand was finalized before.
         */
        ProcessingThread* finalize()
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
                return nullptr;
            finalized_ = true;
            for (auto const& id : permanentTasks_)
                processingThread_->removePermanentTask(id);
            return processingThread_;
        }

      private:
        std::recursive_mutex mutex_{};
        std::atomic_bool finalized_{false};
        ProcessingThread* processingThread_{};
        std::set<ProcessingThread::PermanentTaskId> permanentTasks_{};
    };
}