#include <ftp/remote/ftp_connection.hpp>
#include <ftp/client/without_throwing.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
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

        LoginFailure failureOf(RemoteError error)
        {
            return LoginFailure{.error = std::move(error)};
        }

        LoginFailure exceptionFailure(std::string const& what, std::exception_ptr const& exception)
        {
            return failureOf(errorOf(
                RemoteErrorKind::Failure,
                fmt::format("{} ended with an exception: {}", what, Utility::errorMessage(exception))
            ));
        }

        /**
         * @brief Looks up the server of @p pool and reaches it, on the strand this runs on.
         */
        asio::awaitable<Reachability> lookUpAndReach(std::shared_ptr<Resolver> resolver, PoolOptions pool)
        {
            const auto executor = co_await asio::this_coro::executor;
            auto [lookupException, found] = co_await asio::co_spawn(
                executor,
                Detail::withoutThrowing(resolver->resolve(pool.host, pool.port, asio::use_awaitable)),
                asio::cancel_after(pool.resolveTimeout, Detail::useTuple)
            );
            if (lookupException)
                co_return std::unexpected(exceptionFailure("The lookup of the server", lookupException));
            if (!found)
                co_return std::unexpected(failureOf(std::move(found).error()));

            const auto connection = ControlConnection::create(executor, pool.control);
            auto [reachException, reached] = co_await asio::co_spawn(
                connection->strand(),
                Detail::withoutThrowing(reach(connection, *std::move(found), std::move(pool.login))),
                Detail::useTuple
            );
            if (reachException)
                co_return std::unexpected(exceptionFailure("Reaching the server", reachException));
            if (!reached)
            {
                auto failure = failureOf(std::move(reached).error());
                if (failure.error.kind == RemoteErrorKind::TrustRequired)
                    failure.untrustedCertificate = connection->untrustedCertificate();
                co_return std::unexpected(std::move(failure));
            }
            co_return std::move(reached->certificate);
        }

        /**
         * @brief What a reachServer() call keeps until it completed: its result, and how a stop request cancels it.
         */
        struct Reaching
        {
            std::promise<Reachability> promise{};
            /** @brief Emitted on the strand of the reach only. */
            asio::cancellation_signal stopSignal{};
            std::optional<std::stop_callback<std::function<void()>>> onStop{};
        };

        bool isCancelled(Reachability const& reachability)
        {
            return !reachability && reachability.error().error.kind == RemoteErrorKind::Cancelled;
        }

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

    std::future<Reachability> reachServer(
        asio::any_io_executor executor,
        std::shared_ptr<Resolver> resolver,
        PoolOptions pool,
        std::stop_token stop
    )
    {
        const auto reaching = std::make_shared<Reaching>();
        auto future = reaching->promise.get_future();
        const auto backstop = loginLimitsFor(pool).backstop;
        const auto strand = asio::make_strand(executor);
        asio::co_spawn(
            strand,
            Detail::withoutThrowing(lookUpAndReach(std::move(resolver), std::move(pool))),
            asio::cancel_after(
                backstop,
                asio::bind_cancellation_slot(
                    reaching->stopSignal.slot(),
                    [reaching, stop](std::exception_ptr exception, Reachability reachability) noexcept
                    {
                        auto& promise = reaching->promise;
                        if (exception)
                            return promise.set_value(
                                std::unexpected(exceptionFailure("Reaching the server", exception))
                            );
                        // Without a stop the backstop was the cancel, the server took too long to answer.
                        if (isCancelled(reachability) && !stop.stop_requested())
                            return promise.set_value(
                                std::unexpected(
                                    failureOf(errorOf(RemoteErrorKind::TimedOut, "The server did not answer in time"))
                                )
                            );
                        promise.set_value(std::move(reachability));
                    }
                )
            )
        );
        // After the spawn, which connected the slot: a stop requested by now is emitted behind the start.
        reaching->onStop.emplace(
            stop,
            [weak = std::weak_ptr{reaching}, strand]()
            {
                asio::post(
                    strand,
                    [weak]()
                    {
                        if (const auto reaching = weak.lock(); reaching)
                            reaching->stopSignal.emit(asio::cancellation_type::terminal);
                    }
                );
            }
        );
        return future;
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
        // The lost handler is a task of the thread, which takes none any more: the loss is reported from here.
        thread_->onFailure(
            [shared = shared_, weakEngine = std::weak_ptr{engine_}](std::string const& reason)
            {
                const auto what = fmt::format("its processing thread ended on an exception: {}", reason);
                if (const auto engine = weakEngine.lock())
                    engine->fail(what);
                shared->lost(errorOf(RemoteErrorKind::ConnectionLost, fmt::format("The FTP session failed: {}", what)));
            }
        );
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
