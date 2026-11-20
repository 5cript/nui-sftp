#pragma once

#include <ftp/client/login.hpp>
#include <ftp/client/metadata.hpp>
#include <ftp/client/tls.hpp>
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
#include <optional>
#include <vector>

namespace FileTransferProtocol
{
    struct FtpConnectionOptions
    {
        /** @brief The server, the login and the connections of the session. */
        PoolOptions pool{};
        /** @brief The options of every file system of the session. */
        FtpFileSystemOptions fileSystem{};
        /**
         * @brief The bounds of the first login, loginLimitsFor() of the pool when unset. A backstop below the
         *        timeouts of the login's steps ends a slow login as TimedOut in place of the error of its step.
         */
        std::optional<SyncLimits> loginLimits{};
        TeardownOptions teardown{};
    };

    /**
     * @brief Bounds that lie behind every timeout of a login's steps, so the backstop only ends a login that hangs:
     *        the lookup and the connect, both twice for a server whose addresses did not answer, and its commands.
     */
    SyncLimits loginLimitsFor(PoolOptions const& pool);

    /**
     * @brief Why the login of a session failed.
     */
    struct LoginFailure
    {
        RemoteIo::RemoteError error{};
        /**
         * @brief With TrustRequired the certificate the login refused, what a trust prompt shows. Nothing of the
         *        credentials was sent, and this session logs in no more: a user who trusts the certificate gets a
         *        new connection whose TlsConnectionSettings::pinnedPublicKey is its publicKeySha256.
         */
        std::optional<CertificateInfo> untrustedCertificate{};
    };

    /** @brief The certificate a reached server showed, nothing without TLS, or why it could not be reached. */
    using Reachability = std::expected<std::optional<TrustedCertificate>, LoginFailure>;

    /**
     * @brief Looks up the server of @p pool and reaches it on a strand of @p executor, see reach(): whether it answers
     *        and its certificate is trusted, before anybody is asked for a password. Nothing of the credentials is
     *        sent. Bounded like a first login, see loginLimitsFor(). Callable from any thread.
     *
     * @return Completes with the certificate, or the failure: NoConnection or TimedOut for a server that is not
     *         there, TrustRequired with the certificate to ask about, the error of the greeting or of TLS.
     */
    std::future<Reachability>
    reachServer(boost::asio::any_io_executor executor, std::shared_ptr<Resolver> resolver, PoolOptions pool);

    /**
     * @brief One FTP session as a RemoteIo connection: its ProcessingThread, its engine on an executor of the
     *        application, and the file systems opened on it, which share both.
     *
     * Nothing is sent before logIn(). The destructor ends the session on the calling thread, which must be neither an
     * engine thread nor the processing thread: it fails what runs, sends QUIT within its bounds, stops the thread and
     * closes the file systems, so a caller that still holds one finds it closed. A file system and its streams keep
     * the stopped ProcessingThread while they exist.
     */
    class FtpConnection : public RemoteIo::IConnection
    {
      public:
        /** @brief Runs once when the session is lost or failed, never during or after its close. */
        using LostHandler = std::function<void(RemoteIo::RemoteError const&)>;

        /**
         * @param executor The application's executor, the engine makes its strands on it and never runs it.
         * @param resolver The application's resolver.
         * @param onLost Runs on the processing thread, for a loss before start() in start(). A processing thread
         *        that ended on the exception of a task fails the session and reports that loss as its last act.
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
         * @return Completes with what the login learned, or with the error of the lookup or the login, TimedOut past
         *         the login limits, Failure for a second call.
         */
        std::future<std::expected<LoginResult, LoginFailure>> logIn();

        /**
         * @brief Reports the losses from now on, one that came before at once.
         */
        void start() override;

        /**
         * @brief A file system on the session, ready at once. NoConnection before the login succeeded,
         *        ConnectionLost once the session ends, failed or was lost.
         */
        std::future<std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteIo::RemoteError>>
        createFileSystem() override;

      private:
        struct Shared;

        std::vector<std::shared_ptr<FtpFileSystem>> takeFileSystems();

      private:
        const FtpConnectionOptions options_;
        /** @brief Shared with the strands of the file systems, which may outlive the connection. */
        const std::shared_ptr<RemoteIo::ProcessingThread> thread_ = std::make_shared<RemoteIo::ProcessingThread>();
        std::shared_ptr<TaskGate> gate_;
        std::shared_ptr<ServerDialect> dialect_;
        std::shared_ptr<Shared> shared_;
        std::shared_ptr<Engine> engine_;
        std::mutex fileSystemsMutex_{};
        std::vector<std::shared_ptr<FtpFileSystem>> fileSystems_{};
    };
}
