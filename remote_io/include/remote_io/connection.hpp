#pragma once

#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>

#include <expected>
#include <future>
#include <memory>

namespace RemoteIo
{
    /**
     * @brief A logged in connection to a server, whatever protocol it speaks. Its protocol takes a callback for its
     *        loss when it is made.
     */
    class IConnection
    {
      public:
        IConnection() = default;
        virtual ~IConnection() = default;
        IConnection(IConnection const&) = delete;
        IConnection& operator=(IConnection const&) = delete;
        IConnection(IConnection&&) = delete;
        IConnection& operator=(IConnection&&) = delete;

        /**
         * @brief Starts processing the connection.
         */
        virtual void start() = 0;

        /**
         * @brief Opens a file system on the connection, which owns it. Callable from any thread.
         */
        virtual std::future<std::expected<std::weak_ptr<IRemoteFileSystem>, RemoteError>> createFileSystem() = 0;
    };
}
