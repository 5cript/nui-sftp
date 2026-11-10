#pragma once

#include <remote_io/async/processing_thread.hpp>

#include <functional>
#include <shared_mutex>

namespace FileTransferProtocol
{
    /**
     * @brief The way from the engine to the ProcessingThread of its session. Once closed it refuses every push, so a
     *        handler that runs late never touches a thread that stopped. Thread-safe.
     */
    class TaskGate
    {
      public:
        explicit TaskGate(RemoteIo::ProcessingThread& thread);
        TaskGate(TaskGate const&) = delete;
        TaskGate& operator=(TaskGate const&) = delete;
        TaskGate(TaskGate&&) = delete;
        TaskGate& operator=(TaskGate&&) = delete;
        ~TaskGate() = default;

        /**
         * @return False for an empty task, once closed, or when the thread refused the task, which is then destroyed
         *         outside the gate's lock.
         */
        bool push(std::function<void()> task);

        /**
         * @brief The caller runs on the processing thread, false once closed.
         */
        bool onProcessingThread() const;

        /**
         * @brief Waits for the pushes under way and refuses all later ones.
         */
        void close();

      private:
        mutable std::shared_mutex mutex_{};
        RemoteIo::ProcessingThread* thread_;
    };
}
