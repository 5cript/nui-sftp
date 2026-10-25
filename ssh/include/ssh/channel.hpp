#pragma once

#include <libssh/libsshpp.hpp>
#include <libssh/callbacks.h>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <chrono>
#include <future>

namespace SecureShell
{
    class Session;

    class Channel : public std::enable_shared_from_this<Channel>
    {
      public:
        Channel(Session* owner, std::unique_ptr<ProcessingStrand> strand, std::unique_ptr<ssh::Channel>);
        ~Channel();
        Channel(Channel const&) = delete;
        Channel& operator=(Channel const&) = delete;
        // Libssh's channel callbacks point at this object.
        Channel(Channel&&) = delete;
        Channel& operator=(Channel&&) = delete;

        operator ssh::Channel&()
        {
            return *channel_;
        }

        bool close(bool isBackElement = false);

        /**
         * @brief Writes data to the channel.
         *
         * @param data The data to write
         */
        bool write(std::string data);

        struct Dimensions
        {
            int columns;
            int rows;
        };
        /**
         * @brief Resizes the pty.
         *
         * @param cols The new number of columns.
         * @param rows The new number of rows.
         * @return int 0 on success, otherwise an error code.
         */
        std::future<int> resizePty(int cols, int rows);

        /**
         * @brief Resizes the pty.
         *
         * @param dimensions The new dimensions.
         */
        std::future<int> resizePty(Dimensions const& dimensions)
        {
            return resizePty(dimensions.columns, dimensions.rows);
        }

        /**
         * @brief Starts reading and processing the channel.
         *
         * Output reaches the handlers as libssh processes it, whichever call on the session read it from the
         * socket, so the processing thread can sleep until the socket has data.
         *
         * @param onStdout Called on the processing thread with output.
         * @param onStderr Called on the processing thread with error output.
         * @param onExit Called on the processing thread when the channel or the connection ends, unless closed.
         */
        void startReading(
            std::function<void(std::string const&)> onStdout,
            std::function<void(std::string const&)> onStderr,
            std::function<void()> onExit
        );

      private:
        ProcessingThread::PermanentTaskStatus readTask();
        void registerCallbacks();
        void removeCallbacks();
        static int onData(ssh_session session, ssh_channel channel, void* data, uint32_t length, int isStderr, void* self);
        static void onEnd(ssh_session session, ssh_channel channel, void* self);

      private:
        Session* owner_;
        std::unique_ptr<ProcessingStrand> strand_;
        std::unique_ptr<ssh::Channel> channel_;
        std::function<void(std::string const&)> onStdout_{};
        std::function<void(std::string const&)> onStderr_{};
        std::function<void()> onExit_{};
        /** @brief Set by close on the caller's thread, so the read task no longer reports the exit it expects. */
        std::atomic_bool closing_{false};
        /** @brief Registered with libssh by the first read task, only touched on the processing thread. */
        ssh_channel_callbacks_struct callbacks_{};
        bool callbacksRegistered_{false};
    };
}