#include "ftp_session.hpp"

#include <remote_io/remote_error.hpp>

#include <fmt/format.h>

#include <chrono>
#include <future>
#include <utility>

namespace Benchmark
{
    namespace
    {
        using namespace std::chrono_literals;
        using FileTransferProtocol::FtpConnectionOptions;
        using FileTransferProtocol::TlsMode;

        /** @brief Beyond the login's own bounds, which end a login that hangs first. */
        constexpr auto loginWait = 2min;

        std::expected<FtpConnectionOptions, std::string> connectionOptionsOf(FtpSessionOptions const& options)
        {
            FtpConnectionOptions connectionOptions{};
            connectionOptions.pool.host = options.host;
            connectionOptions.pool.port = options.port;
            connectionOptions.pool.login.tlsMode = options.tlsMode;
            connectionOptions.pool.login.credentials.user = options.user;
            connectionOptions.pool.login.credentials.password = options.password;
            connectionOptions.fileSystem.ring = options.ring;
            if (options.tlsMode == TlsMode::None)
                return connectionOptions;

            FileTransferProtocol::TlsContextOptions contextOptions{};
            contextOptions.systemAuthorities = false;
            auto context = FileTransferProtocol::TlsContext::create(std::move(contextOptions));
            if (!context)
                return std::unexpected{fmt::format("no TLS context: {}", context.error().toString())};
            connectionOptions.pool.login.tls = FileTransferProtocol::TlsConnectionSettings{
                .context = std::move(context).value(),
                .serverName = options.host,
                .pinnedPublicKey = options.pinnedPublicKey,
            };
            return connectionOptions;
        }
    }

    EngineThreads::EngineThreads(std::size_t count)
    {
        if (count > 1u)
        {
            pool_.emplace(count);
            return;
        }
        context_.emplace();
        workGuard_.emplace(context_->get_executor());
        runner_ = std::thread{[this]()
                              {
                                  context_->run();
                              }};
    }

    EngineThreads::~EngineThreads()
    {
        // The sessions on these threads are closed by now, what is still queued is abandoned.
        if (pool_)
        {
            pool_->stop();
            pool_->join();
            return;
        }
        workGuard_.reset();
        context_->stop();
        runner_.join();
    }

    boost::asio::any_io_executor EngineThreads::executor()
    {
        if (pool_)
            return pool_->get_executor();
        return context_->get_executor();
    }

    FtpSession::FtpSession(std::size_t engineThreads)
        : threads_{engineThreads}
        , resolver_{std::make_shared<FileTransferProtocol::Resolver>()}
    {}

    std::expected<std::unique_ptr<FtpSession>, std::string> FtpSession::prepare(FtpSessionOptions const& options)
    {
        auto connectionOptions = connectionOptionsOf(options);
        if (!connectionOptions)
            return std::unexpected{connectionOptions.error()};
        std::unique_ptr<FtpSession> session{new FtpSession{options.engineThreads}};
        session->connection_ = std::make_unique<FileTransferProtocol::FtpConnection>(
            session->threads_.executor(), session->resolver_, std::move(connectionOptions).value()
        );
        return session;
    }

    std::expected<FileTransferProtocol::LoginResult, FileTransferProtocol::LoginFailure> FtpSession::logIn()
    {
        auto login = connection_->logIn();
        if (login.wait_for(loginWait) != std::future_status::ready)
        {
            FileTransferProtocol::LoginFailure failure{};
            failure.error.kind = RemoteIo::RemoteErrorKind::TimedOut;
            failure.error.protocol = RemoteIo::Protocol::Ftp;
            failure.error.message = "the login did not complete";
            return std::unexpected{std::move(failure)};
        }
        return login.get();
    }

    std::expected<std::unique_ptr<FtpSession>, std::string> FtpSession::open(FtpSessionOptions const& options)
    {
        auto session = prepare(options);
        if (!session)
            return session;
        if (const auto loggedIn = (*session)->logIn(); !loggedIn)
            return std::unexpected{fmt::format("the FTP login failed: {}", loggedIn.error().error.toString())};

        (*session)->connection_->start();
        const auto fileSystem = (*session)->connection_->createFileSystem().get();
        if (!fileSystem)
            return std::unexpected{fmt::format("no FTP file system: {}", fileSystem.error().toString())};
        (*session)->fileSystem_ = fileSystem->lock();
        if (!(*session)->fileSystem_)
            return std::unexpected<std::string>{"the FTP file system closed at once"};
        return session;
    }

    std::expected<FileTransferProtocol::CertificateInfo, std::string>
    FtpSession::refusedCertificate(FtpSessionOptions options)
    {
        options.pinnedPublicKey.reset();
        auto session = prepare(options);
        if (!session)
            return std::unexpected{session.error()};
        const auto loggedIn = (*session)->logIn();
        if (loggedIn)
            return std::unexpected<std::string>{"the server's certificate was trusted without a pin"};
        if (!loggedIn.error().untrustedCertificate)
            return std::unexpected{fmt::format("the FTPS login failed: {}", loggedIn.error().error.toString())};
        return *loggedIn.error().untrustedCertificate;
    }
}
