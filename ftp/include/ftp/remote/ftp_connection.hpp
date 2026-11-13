#pragma once

#include <ftp/client/metadata.hpp>
#include <ftp/engine/call.hpp>
#include <ftp/engine/engine.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <ftp/pool/pool.hpp>
#include <ftp/remote/file_system_session.hpp>
#include <ftp/remote/ftp_file_system.hpp>
#include <remote_io/async/processing_thread.hpp>
#include <remote_io/connection.hpp>
#include <remote_io/remote_error.hpp>
#include <remote_io/remote_file_system.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace FileTransferProtocol
{
    struct FtpConnectionOptions
    {
        /** @brief The server, the login and the connections of the session. */
        PoolOptions pool{};
        /** @brief The options of every file system of the session. */
        FtpFileSystemOptions fileSystem{};
        /** @brief The bounds of the first login, beyond the timeouts of its connect and commands. */
        SyncLimits loginLimits{.backstop = std::chrono::seconds{60}, .drain = std::chrono::seconds{1}};
        TeardownOptions teardown{};
    };

    /**
     * @brief One FTP session as a RemoteIo connection: its ProcessingThread, its engine on an executor of the
     *        application, and the file systems opened on it, which share both.
     *
     * Nothing is sent before logIn(). The destructor ends the session on the calling thread, which must be neither an
     * engine thread nor the processing thread: it fails what runs, sends QUIT within its bounds, stops the thread and
     * closes the file systems, so a caller that still holds one finds it closed.
     */
    class FtpConnection : public RemoteIo::IConnection
    {
      public:
        /** @brief Runs once when the session is lost or failed, never during or after its close. */
        using LostHandler = std::function<void(RemoteIo::RemoteError const&)>;

        /**
         * @param executor The application's executor, the engine makes its strands on it and never runs it.
         * @param resolver The application's resolver.
         * @param onLost Runs on the processing thread, for a loss before start() in start().
         */
        FtpConnection(
            boost::asio::any_io_executor executor,
            std::shared_ptr<Resolver> resolver,
            FtpConnectionOptions options,
            LostHandler onLost = {}
        );
        ~FtpConnection() override;
        FtpConnection(FtpConnection const&) = delete;
        FtpConnection& operator=(FtpConnection const&) = delete;
        FtpConnection(FtpConnection&&) = delete;
        FtpConnection& operator=(FtpConnection&&) = delete;

        /**
         * @brief Looks up the server and logs in the first connection, once, from any thread. The destructor fails a
         *        login that still runs.
         *
         * @return Completes with the error of the lookup or the login, TimedOut past the login limits, Failure for a
         *         second call.
         */
        std::future<std::expected<void, RemoteIo::RemoteError>> logIn();

        /**
         * @brief Reports the losses from now on, one that came before at once.
         */
        void start() override;

        /**
         * @brief A file system on the session, ready at once. NoConnection before the login succeeded,
         *        ConnectionLost once the session ends or failed.
         */
        std::future<std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteIo::RemoteError>>
        createFileSystem() override;

      private:
        struct Shared;

        std::vector<std::shared_ptr<FtpFileSystem>> takeFileSystems();

      private:
        const FtpConnectionOptions options_;
        RemoteIo::ProcessingThread thread_{};
        std::shared_ptr<TaskGate> gate_;
        std::shared_ptr<ServerDialect> dialect_;
        std::shared_ptr<Shared> shared_;
        std::shared_ptr<Engine> engine_;
        std::mutex fileSystemsMutex_{};
        std::vector<std::shared_ptr<FtpFileSystem>> fileSystems_{};
    };
}
