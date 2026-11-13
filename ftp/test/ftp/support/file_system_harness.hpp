#pragma once

#include <remote_io/remote_file_system.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace FileTransferProtocol::Test
{
    /**
     * @brief An FtpFileSystem logged in to a server, with the engine and the threads it runs on, for tests that see
     *        only the RemoteIo interface.
     */
    class FileSystemHarness
    {
      public:
        struct Options
        {
            std::string host{};
            std::uint16_t port{0u};
            std::size_t transferConnections{1u};
        };

        /**
         * @brief Logs in as user with password and opens the file system.
         *
         * @return The harness, or why the login failed.
         */
        static std::expected<std::unique_ptr<FileSystemHarness>, std::string> connect(Options const& options);

        /**
         * @brief Closes the file system, then the session, which sends QUIT.
         */
        ~FileSystemHarness();
        FileSystemHarness(FileSystemHarness const&) = delete;
        FileSystemHarness& operator=(FileSystemHarness const&) = delete;
        FileSystemHarness(FileSystemHarness&&) = delete;
        FileSystemHarness& operator=(FileSystemHarness&&) = delete;

        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem() const;

      private:
        class Implementation;
        explicit FileSystemHarness(std::unique_ptr<Implementation> implementation);

        std::unique_ptr<Implementation> impl_;
    };
}
