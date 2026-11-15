#include <ftp/remote/ftp_connection.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Detail::errorOf;
        using RemoteIo::RemoteError;
        using RemoteIo::RemoteErrorKind;

        template <typename ValueT>
        std::future<ValueT> readyFuture(ValueT value)
        {
            std::promise<ValueT> promise{};
            promise.set_value(std::move(value));
            return promise.get_future();
        }

        /**
         * @brief What a login waits for with the command timeout at most: the greeting, AUTH TLS and its handshake,
         *        USER, PASS, ACCT, PBSZ, PROT, FEAT, OPTS, TYPE and PWD.
         */
        constexpr auto loginReplies = 12;

        /**
         * @brief A strand of @p thread that keeps it, for a file system held past its connection.
         */
        std::shared_ptr<RemoteIo::ProcessingStrand> strandKeeping(std::shared_ptr<RemoteIo::ProcessingThread> thread)
        {
            struct KeptStrand
            {
                std::shared_ptr<RemoteIo::ProcessingThread> thread;
                std::unique_ptr<RemoteIo::ProcessingStrand> strand;
            };
            auto strand = thread->createStrand();
            const auto kept = std::make_shared<KeptStrand>(std::move(thread), std::move(strand));
            return std::shared_ptr<RemoteIo::ProcessingStrand>{kept, kept->strand.get()};
        }
    }

    SyncLimits loginLimitsFor(PoolOptions const& pool)
    {
        return SyncLimits{
            .backstop =
                2 * (pool.resolveTimeout + pool.control.connectTimeout) + loginReplies * pool.control.commandTimeout,
            .drain = std::chrono::seconds{1},
        };
    }

    struct FtpConnection::Shared
    {
        explicit Shared(LostHandler handler)
            : onLost{std::move(handler)}
        {}

        void lost(RemoteError const& error)
        {
            std::unique_lock lock{mutex};
            if (closing || reported)
                return;
            if (!started)
            {
                if (!pendingLoss)
                    pendingLoss = error;
                return;
            }
            reported = true;
            const auto handler = onLost;
            lock.unlock();
            if (handler)
                handler(error);
        }

        void start()
        {
            std::unique_lock lock{mutex};
            if (std::exchange(started, true) || closing || reported || !pendingLoss)
                return;
            reported = true;
            const auto error = *std::exchange(pendingLoss, std::nullopt);
            const auto handler = onLost;
            lock.unlock();
            if (handler)
                handler(error);
        }

        void close()
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }

        bool beginLogin()
        {
            std::scoped_lock lock{mutex};
            return !std::exchange(loginStarted, true);
        }

        void loggedIn(std::string directory)
        {
            std::scoped_lock lock{mutex};
            homeDirectory = std::move(directory);
        }

        std::optional<std::string> loggedInHome() const
        {
            std::scoped_lock lock{mutex};
            return homeDirectory;
        }

        void refused(std::optional<CertificateInfo> certificate)
        {
            std::scoped_lock lock{mutex};
            refusedCertificate = std::move(certificate);
        }

        std::optional<CertificateInfo> refusedByLogin() const
        {
            std::scoped_lock lock{mutex};
            return refusedCertificate;
        }

        /**
         * @brief The login of the session on its strand, which keeps the certificate a TrustRequired one refused.
         */
        static asio::awaitable<Engine::Result<LoginResult>>
        logInWith(std::shared_ptr<Shared> shared, std::shared_ptr<Pool> pool)
        {
            if (!pool)
                co_return std::unexpected(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended"));
            auto loggedIn = co_await pool->start();
            if (!loggedIn && loggedIn.error().kind == RemoteErrorKind::TrustRequired)
                shared->refused(pool->untrustedCertificate());
            co_return loggedIn;
        }

        mutable std::mutex mutex{};
        const LostHandler onLost;
        bool started{false};
        bool closing{false};
        bool reported{false};
        bool loginStarted{false};
        std::optional<RemoteError> pendingLoss{};
        /** @brief Set once the login succeeded, empty when the server told no directory. */
        std::optional<std::string> homeDirectory{};
        std::optional<CertificateInfo> refusedCertificate{};
    };

    FtpConnection::FtpConnection(
        asio::any_io_executor executor,
        std::shared_ptr<Resolver> resolver,
        FtpConnectionOptions options,
        LostHandler onLost
    )
        : options_{std::move(options)}
        , gate_{std::make_shared<TaskGate>(*thread_)}
        , dialect_{std::make_shared<ServerDialect>()}
        , shared_{std::make_shared<Shared>(std::move(onLost))}
        , engine_{Engine::create(
              std::move(executor),
              std::move(resolver),
              gate_,
              EngineOptions{.pool = options_.pool},
              [shared = shared_](RemoteError const& error)
              {
                  shared->lost(error);
              }
          )}
    {
        thread_->start();
    }

    FtpConnection::~FtpConnection()
    {
        shared_->close();
        // The engine first: it fails the calls running now, so the final tasks of the file systems wait behind none.
        if (!engine_->close(options_.teardown))
            Log::warn("FTP: Work of the session ran on past its close");
        for (auto const& fileSystem : takeFileSystems())
            fileSystem->close();
    }

    std::future<std::expected<LoginResult, LoginFailure>> FtpConnection::logIn()
    {
        const auto promise = std::make_shared<std::promise<std::expected<LoginResult, LoginFailure>>>();
        auto future = promise->get_future();
        if (!shared_->beginLogin())
        {
            promise->set_value(
                std::unexpected(
                    LoginFailure{.error = errorOf(RemoteErrorKind::Failure, "The FTP session logs in only once")}
                )
            );
            return future;
        }
        engine_->run<LoginResult>(
            [shared = shared_, weak = std::weak_ptr{engine_}](std::shared_ptr<CallScope> const&)
            {
                const auto engine = weak.lock();
                return Shared::logInWith(shared, engine ? engine->pool() : nullptr);
            },
            options_.loginLimits.value_or(loginLimitsFor(options_.pool)),
            [shared = shared_, dialect = dialect_, promise](Engine::Result<LoginResult> result)
            {
                if (!result)
                {
                    auto failure = LoginFailure{.error = std::move(result).error()};
                    // Only the refusal itself has a certificate to ask about, a login cut short has none.
                    if (failure.error.kind == RemoteErrorKind::TrustRequired)
                        failure.untrustedCertificate = shared->refusedByLogin();
                    promise->set_value(std::unexpected(std::move(failure)));
                    return;
                }
                dialect->learnFrom(result->features);
                shared->loggedIn(result->homeDirectory);
                promise->set_value(std::move(*result));
            }
        );
        return future;
    }

    void FtpConnection::start()
    {
        shared_->start();
    }

    std::future<std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteError>>
    FtpConnection::createFileSystem()
    {
        using Result = std::expected<std::weak_ptr<RemoteIo::IRemoteFileSystem>, RemoteError>;
        if (engine_->stopping() || engine_->failed())
            return readyFuture<Result>(
                std::unexpected(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session ended"))
            );
        auto homeDirectory = shared_->loggedInHome();
        if (!homeDirectory)
            return readyFuture<Result>(
                std::unexpected(errorOf(RemoteErrorKind::NoConnection, "The FTP session is not logged in"))
            );
        if (engine_->lost())
            return readyFuture<Result>(
                std::unexpected(errorOf(RemoteErrorKind::ConnectionLost, "The FTP session was lost"))
            );
        auto fileSystem = FtpFileSystem::create(
            FileSystemSession{
                .engine = engine_,
                .dialect = dialect_,
                .homeDirectory = std::move(*homeDirectory),
                .parallelTransfers = options_.pool.transferConnections,
                .probesEntries = dialect_->probesEntries(),
            },
            strandKeeping(thread_),
            options_.fileSystem
        );
        std::weak_ptr<RemoteIo::IRemoteFileSystem> handle = fileSystem;
        {
            std::scoped_lock lock{fileSystemsMutex_};
            std::erase_if(
                fileSystems_,
                [](auto const& open)
                {
                    return open->closed();
                }
            );
            fileSystems_.push_back(std::move(fileSystem));
        }
        return readyFuture<Result>(std::move(handle));
    }

    std::vector<std::shared_ptr<FtpFileSystem>> FtpConnection::takeFileSystems()
    {
        std::scoped_lock lock{fileSystemsMutex_};
        return std::exchange(fileSystems_, {});
    }
}
