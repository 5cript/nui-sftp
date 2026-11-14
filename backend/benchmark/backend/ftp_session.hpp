#pragma once

#include <ftp/client/login.hpp>
#include <ftp/client/tls.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/ring_options.hpp>
#include <ftp/remote/ftp_connection.hpp>
#include <remote_io/remote_file_system.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace Benchmark
{
    /**
     * @brief The threads the FTP engine of a session runs on, as the app's engine context has them: an io_context on
     *        one thread, or a thread_pool of more.
     */
    class EngineThreads
    {
      public:
        explicit EngineThreads(std::size_t count);
        ~EngineThreads();
        EngineThreads(EngineThreads const&) = delete;
        EngineThreads& operator=(EngineThreads const&) = delete;
        EngineThreads(EngineThreads&&) = delete;
        EngineThreads& operator=(EngineThreads&&) = delete;

        boost::asio::any_io_executor executor();

      private:
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

        std::optional<boost::asio::io_context> context_{};
        std::optional<WorkGuard> workGuard_{};
        std::thread runner_{};
        std::optional<boost::asio::thread_pool> pool_{};
    };

    struct FtpSessionOptions
    {
        std::string host{};
        std::uint16_t port{21u};
        std::string user{};
        std::string password{};
        FileTransferProtocol::TlsMode tlsMode{FileTransferProtocol::TlsMode::None};
        /** @brief Trusts the server's certificate by its public key, see FtpSession::refusedCertificate(). */
        std::optional<FileTransferProtocol::Sha256Digest> pinnedPublicKey{};
        std::size_t engineThreads{2u};
        FileTransferProtocol::RingOptions ring{};
    };

    /**
     * @brief One FTP session as the app opens it: an FtpConnection on engine threads of its own, logged in, with one
     *        file system. The file system goes first, then the connection, the threads last.
     */
    class FtpSession
    {
      public:
        static std::expected<std::unique_ptr<FtpSession>, std::string> open(FtpSessionOptions const& options);

        /**
         * @brief The certificate the server at @p options shows, learned as a trust prompt learns it: from a login
         *        that refuses it because no authority vouches for it.
         */
        static std::expected<FileTransferProtocol::CertificateInfo, std::string>
        refusedCertificate(FtpSessionOptions options);

        ~FtpSession() = default;
        FtpSession(FtpSession const&) = delete;
        FtpSession& operator=(FtpSession const&) = delete;
        FtpSession(FtpSession&&) = delete;
        FtpSession& operator=(FtpSession&&) = delete;

        std::shared_ptr<RemoteIo::IRemoteFileSystem> const& fileSystem() const noexcept
        {
            return fileSystem_;
        }

      private:
        explicit FtpSession(std::size_t engineThreads);

        static std::expected<std::unique_ptr<FtpSession>, std::string> prepare(FtpSessionOptions const& options);
        std::expected<FileTransferProtocol::LoginResult, FileTransferProtocol::LoginFailure> logIn();

      private:
        EngineThreads threads_;
        std::shared_ptr<FileTransferProtocol::Resolver> resolver_;
        std::unique_ptr<FileTransferProtocol::FtpConnection> connection_{};
        std::shared_ptr<RemoteIo::IRemoteFileSystem> fileSystem_{};
    };
}
