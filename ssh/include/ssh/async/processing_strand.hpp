#pragma once

#include <ssh/async/processing_thread.hpp>

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
            auto result = processingThread_->pushPermanentTask(
                [task = std::move(task), this](ProcessingThread::PermanentTaskId id)
                {
                    const auto result = task();
                    if (!result)
                        permanentTasks_.erase(id);
                    return result;
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
            std::scoped_lock lock(mutex_);
            if (finalized_)
                return;
            finalized_ = true;
            for (auto const& id : permanentTasks_)
            {
                processingThread_->removePermanentTask(id);
            }
            processingThread_->pushTask(std::move(task));
        }

        /**
         * @brief Works like pushFinalTask but returns a future for the return value of the task.
         *
         * @param task The task to run.
         */
        template <typename FunctionT>
        auto pushFinalPromiseTask(FunctionT&& func) -> std::future<std::invoke_result_t<std::decay_t<FunctionT>>>
        {
            std::scoped_lock lock(mutex_);
            if (finalized_)
            {
                std::promise<std::invoke_result_t<std::decay_t<FunctionT>>> promise{};
                promise.set_exception(
                    std::make_exception_ptr(std::runtime_error("Cannot push task to finalized strand."))
                );
                return promise.get_future();
            }
            finalized_ = true;
            for (auto const& id : permanentTasks_)
            {
                processingThread_->removePermanentTask(id);
            }
            return processingThread_->pushPromiseTask(std::forward<FunctionT>(func));
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

        bool withinProcessingThread() const noexcept
        {
            return processingThread_->withinProcessingThread();
        }

        bool isFinalized() const noexcept
        {
            return finalized_;
        }

      private:
        std::recursive_mutex mutex_{};
        bool finalized_ = false;
        ProcessingThread* processingThread_{};
        std::set<ProcessingThread::PermanentTaskId> permanentTasks_{};
    };
}