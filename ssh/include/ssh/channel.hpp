#pragma once

#include <libssh/libsshpp.hpp>
#include <ssh/async/processing_thread.hpp>
#include <ssh/async/processing_strand.hpp>

#include <memory>
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
        Channel(Channel&&);
        Channel& operator=(Channel&&);

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
         * @param onStdout
         * @param onStderr
         * @param onExit
         */
        void startReading(
            std::function<void(std::string const&)> onStdout,
            std::function<void(std::string const&)> onStderr,
            std::function<void()> onExit
        );

      private:
        bool readTask(std::chrono::milliseconds pollTimeout = std::chrono::milliseconds{0});

      private:
        Session* owner_;
        std::unique_ptr<ProcessingStrand> strand_;
        std::unique_ptr<ssh::Channel> channel_;
        std::function<void(std::string const&)> onStdout_{};
        std::function<void(std::string const&)> onStderr_{};
        std::function<void()> onExit_{};
    };
}