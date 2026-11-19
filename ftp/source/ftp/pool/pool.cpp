#include <ftp/pool/pool.hpp>
#include <ftp/client/without_throwing.hpp>
#include <ftp/protocol/command.hpp>
#include <log/log.hpp>
#include "../client/client_support.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <exception>
#include <string_view>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        namespace asio = boost::asio;
        using Clock = std::chrono::steady_clock;
        using RemoteIo::RemoteErrorKind;
        using Detail::describe;
        using Detail::errorOf;
        using Detail::isCancelled;
        using Detail::useTuple;
        using Detail::withoutThrowing;
        using LoginOutcome =
            std::pair<std::expected<LoginResult, RemoteIo::RemoteError>, std::optional<CertificateInfo>>;

        /**
         * @brief Another login may work after a failure like this one: the server or the network may be back, or a
         *        slot free.
         */
        bool worthAnotherLogin(RemoteIo::RemoteError const& error)
        {
            switch (error.kind)
            {
                case RemoteErrorKind::NoConnection:
                case RemoteErrorKind::ConnectionLost:
                case RemoteErrorKind::ConnectionLimit:
                case RemoteErrorKind::TimedOut:
                    return true;
                default:
                    return false;
            }
        }

        /**
         * @brief No endpoint took the connection in time, a new lookup may find the server elsewhere.
         */
        bool reachedNoEndpoint(RemoteIo::RemoteError const& error)
        {
            return error.kind == RemoteErrorKind::NoConnection || error.kind == RemoteErrorKind::TimedOut;
        }

        RemoteIo::RemoteError closing()
        {
            return errorOf(RemoteErrorKind::ConnectionLost, "The FTP session is closing");
        }

        RemoteIo::RemoteError cancelled(std::string_view what)
        {
            return errorOf(RemoteErrorKind::Cancelled, fmt::format("{} was cancelled", what));
        }

        /**
         * @brief QUIT within @p timeout, then a close in any case.
         */
        asio::awaitable<void> quit(std::shared_ptr<ControlConnection> control, std::chrono::milliseconds timeout)
        {
            co_await asio::co_spawn(
                control->strand(),
                withoutThrowing(control->execute(Command{.verb = Verb::Quit})),
                asio::cancel_after(timeout, useTuple)
            );
            control->close();
        }
    }

    struct Pool::Waiter
    {
        explicit Waiter(asio::any_io_executor executor)
            : timer{std::move(executor), Clock::time_point::max()}
        {}

        /** @brief Its wait ends when the pool decided, or to look at the deadline again. */
        asio::steady_timer timer;
        std::optional<ConnectionResult> outcome{};
        /** @brief While the pool is Degraded the wait ends here. */
        std::optional<Clock::time_point> deadline{};
    };

    std::shared_ptr<Pool> Pool::create(
        asio::strand<asio::any_io_executor> strand,
        std::shared_ptr<Resolver> resolver,
        PoolOptions options,
        LostHandler onLost
    )
    {
        auto pool = std::make_shared<Pool>(
            Private{}, std::move(strand), std::move(resolver), std::move(options), std::move(onLost)
        );
        // Work in the background keeps the pool, so the end of the owner's handle is what tells that the owner is gone.
        const auto self = pool.get();
        return std::shared_ptr<Pool>{
            self,
            [pool = std::move(pool)](Pool*)
            {
                asio::dispatch(
                    pool->strand_,
                    [pool]()
                    {
                        pool->abandon();
                    }
                );
            }
        };
    }

    Pool::Pool(
        Private,
        asio::strand<asio::any_io_executor> strand,
        std::shared_ptr<Resolver> resolver,
        PoolOptions options,
        LostHandler onLost
    )
        : strand_{std::move(strand)}
        , resolver_{std::move(resolver)}
        , options_{std::move(options)}
        , onLost_{std::move(onLost)}
        , passiveChoice_{std::make_shared<PassiveChoice>()}
        , reloginTimer_{strand_}
        , limitTimer_{strand_}
        , connectionLimit_{options_.transferConnections + 1u}
    {
        assert(options_.transferConnections > 0u && "a pool needs a connection for transfers");
    }

    Pool::~Pool()
    {
        closeConnections();
    }

    asio::strand<asio::any_io_executor> const& Pool::strand() const
    {
        return strand_;
    }

    std::optional<CertificateInfo> const& Pool::untrustedCertificate() const
    {
        return untrustedCertificate_;
    }

    PoolHealth Pool::health() const
    {
        return health_;
    }

    std::size_t Pool::connectionLimit() const
    {
        return connectionLimit_;
    }

    asio::awaitable<std::expected<LoginResult, RemoteIo::RemoteError>> Pool::start()
    {
        assert(strand_.running_in_this_thread() && "the pool runs on its strand");
        assert(!started_ && "a pool starts once");
        started_ = true;
        const auto failStart = [this](RemoteIo::RemoteError error)
        {
            lostReason_ = error;
            health_ = PoolHealth::Lost;
            return std::unexpected(std::move(error));
        };

        auto found = co_await lookUp();
        if (!found)
            co_return failStart(found.error());
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return failStart(cancelled("The start of the FTP session"));
        if (closed_)
            co_return failStart(closing());
        endpoints_ = std::move(*found);

        auto connection = co_await logInConnection(true);
        if (!connection)
            co_return failStart(connection.error());
        if (closed_)
        {
            (*connection)->control->close();
            co_return failStart(closing());
        }
        auto login = (*connection)->login;
        adoptMetadata(std::move(*connection));
        co_return login;
    }

    asio::awaitable<Pool::LeaseResult> Pool::acquireCommand(std::shared_ptr<ControlConnection> held)
    {
        assert(strand_.running_in_this_thread() && "the pool runs on its strand");
        // Each round hands out a connection, waits, or drops a connection that ended, so the rounds run out.
        std::optional<LeaseResult> outcome{};
        while (!outcome)
        {
            if (const auto refused = refusal())
                outcome.emplace(std::unexpected(*refused));
            else if (metadataFree())
                outcome = co_await leaseIfOpen(metadata_);
            else if (const auto connection = idleTransfer())
                outcome = co_await leaseIfOpen(connection);
            else if (held && metadata_ && metadata_->control == held)
            {
                outcome.emplace(
                    std::unexpected(errorOf(RemoteErrorKind::Busy, "Every connection of the FTP session is in use"))
                );
            }
            else
            {
                connectMetadataIfGone();
                outcome = co_await waitForMetadata();
            }
        }
        co_return std::move(*outcome);
    }

    asio::awaitable<Pool::LeaseResult> Pool::acquireTransfer(Wake wakeWhenFree)
    {
        assert(strand_.running_in_this_thread() && "the pool runs on its strand");
        // One login per call, a server that refuses every time costs one refusal per call. The other rounds hand out a
        // connection or drop one that ended.
        auto mayLogIn = true;
        std::optional<LeaseResult> outcome{};
        while (!outcome)
        {
            if (const auto refused = refusal())
                outcome.emplace(std::unexpected(*refused));
            else if (const auto connection = idleTransfer())
                outcome = co_await leaseIfOpen(connection);
            else if (mayLogIn && transfers_.size() + transfersConnecting_ < transferCapacity())
            {
                mayLogIn = false;
                outcome = co_await logInTransfer();
            }
            else if (transferCapacity() == 0u && metadataFree())
                outcome = co_await leaseIfOpen(metadata_);
            else
            {
                if (transferCapacity() == 0u)
                    connectMetadataIfGone();
                if (wakeWhenFree)
                    wakes_.push_back(std::move(wakeWhenFree));
                outcome.emplace(
                    std::unexpected(errorOf(RemoteErrorKind::Busy, "Every connection of the FTP session is in use"))
                );
            }
        }
        co_return std::move(*outcome);
    }

    asio::awaitable<void> Pool::close()
    {
        assert(strand_.running_in_this_thread() && "the pool runs on its strand");
        if (closed_)
            co_return;
        closed_ = true;
        reloginTimer_.cancel();
        limitTimer_.cancel();
        failCommandWaiters(closing());
        fireWakes();
        for (auto const& connection : connecting_)
            connection->control->close();

        auto connections = transfers_;
        if (metadata_)
            connections.push_back(metadata_);
        std::vector<std::shared_ptr<PooledConnection>> idleConnections{};
        for (auto const& connection : connections)
        {
            connection->dropped = true;
            ++connection->idleGeneration;
            connection->idleTimer.cancel();
            if (connection->leased)
                connection->control->close();
            else
                idleConnections.push_back(connection);
        }
        metadata_.reset();
        transfers_.clear();
        if (isCancelled(co_await asio::this_coro::cancellation_state))
        {
            for (auto const& connection : idleConnections)
                connection->control->close();
            co_return;
        }
        co_await quitAll(std::move(idleConnections));
    }

    std::shared_ptr<PooledConnection> Pool::makeConnection(std::string name)
    {
        auto connection = std::make_shared<PooledConnection>(
            std::move(name), ControlConnection::create(strand_.get_inner_executor(), options_.control), strand_
        );
        connection->control->onEnd(
            [weakPool = weak_from_this(),
                weakConnection = std::weak_ptr{connection}](RemoteIo::RemoteError const& error)
            {
                const auto pool = weakPool.lock();
                if (!pool)
                    return;
                asio::post(
                    pool->strand_,
                    [pool, weakConnection, error]()
                    {
                        if (const auto connection = weakConnection.lock())
                            pool->takeEnd(connection, error);
                    }
                );
            }
        );
        return connection;
    }

    asio::awaitable<ResolveResult> Pool::lookUp()
    {
        auto [exception, found] = co_await asio::co_spawn(
            strand_,
            withoutThrowing(resolver_->resolve(options_.host, options_.port, asio::use_awaitable)),
            asio::cancel_after(options_.resolveTimeout, useTuple)
        );
        const auto cancelledHere = isCancelled(co_await asio::this_coro::cancellation_state);
        if (cancelledHere && (exception || !found))
            co_return std::unexpected(cancelled("The lookup of the server"));
        if (exception || (!found && found.error().kind == RemoteErrorKind::Cancelled))
            co_return std::unexpected(errorOf(RemoteErrorKind::TimedOut, "The lookup of the server took too long"));
        co_return std::move(found);
    }

    asio::awaitable<Pool::ConnectionResult> Pool::logInConnection(bool metadata)
    {
        const auto name = metadata ? std::string{"C0"} : fmt::format("C{}", ++transferNumber_);
        auto connection = co_await logInAs(name);
        if (!connection && reachedNoEndpoint(connection.error()) && !closed_ &&
            !isCancelled(co_await asio::this_coro::cancellation_state))
        {
            // Every endpoint failed, the server may have moved. The same addresses in another order, as a DNS round
            // robin answers, are the same server.
            const auto found = co_await lookUp();
            const auto moved = found && !std::ranges::is_permutation(*found, endpoints_);
            if (moved && !closed_ && !isCancelled(co_await asio::this_coro::cancellation_state))
            {
                Log::info("FTP: The server's addresses changed, logging in at the new ones");
                endpoints_ = *found;
                connection = co_await logInAs(name);
            }
        }
        if (!connection && connection.error().kind == RemoteErrorKind::ConnectionLimit)
            learnLimit();
        co_return connection;
    }

    asio::awaitable<Pool::ConnectionResult> Pool::logInAs(std::string name)
    {
        const auto connection = makeConnection(std::move(name));
        connecting_.push_back(connection);
        const auto control = connection->control;
        auto [exception, outcome] = co_await asio::co_spawn(
            control->strand(),
            [control, endpoints = endpoints_, options = options_.login]() -> asio::awaitable<LoginOutcome>
            {
                // As withoutThrowing(), which cannot wrap a coroutine that lives in its lambda.
                co_await asio::this_coro::throw_if_cancelled(false);
                auto loggedIn = co_await logIn(control, endpoints, options);
                std::optional<CertificateInfo> refused{};
                if (!loggedIn && loggedIn.error().kind == RemoteErrorKind::TrustRequired)
                    refused = control->untrustedCertificate();
                co_return LoginOutcome{std::move(loggedIn), std::move(refused)};
            },
            useTuple
        );
        std::erase(connecting_, connection);
        if (exception)
        {
            control->close();
            co_return std::unexpected(errorOf(
                RemoteErrorKind::Failure, fmt::format("The login ended with an exception: {}", describe(exception))
            ));
        }
        auto& [loggedIn, refused] = outcome;
        if (refused)
            untrustedCertificate_ = std::move(refused);
        if (!loggedIn)
            co_return std::unexpected(std::move(loggedIn.error()));

        connection->login = std::move(*loggedIn);
        connection->transferSession = TransferSession{
            .control = control,
            .passiveChoice = passiveChoice_,
            .tls = options_.login.tlsMode == TlsMode::None ? std::nullopt : options_.login.tls,
            .restartStream = connection->login.features.has("REST", "STREAM"),
        };
        Log::debug("FTP: Logged in {}", connection->name);
        co_return connection;
    }

    asio::awaitable<std::optional<Pool::LeaseResult>> Pool::logInTransfer()
    {
        ++transfersConnecting_;
        auto connection = co_await logInConnection(false);
        --transfersConnecting_;
        if (connection && (closed_ || health_ == PoolHealth::Lost))
        {
            (*connection)->control->close();
            co_return LeaseResult{std::unexpected(*refusal())};
        }
        if (connection)
        {
            transfers_.push_back(*connection);
            // A refusal of another login lowered the limit while this one was under way, the server took it all the
            // same.
            if (connectionLimit_ < transfers_.size() + 1u)
                connectionLimit_ = transfers_.size() + 1u;
            co_return LeaseResult{lease(*connection)};
        }
        // The refusal lowered the limit, C0 or a wait may be the way now.
        if (connection.error().kind == RemoteErrorKind::ConnectionLimit)
            co_return std::nullopt;
        co_return LeaseResult{std::unexpected(std::move(connection.error()))};
    }

    asio::awaitable<std::optional<Pool::LeaseResult>> Pool::leaseIfOpen(std::shared_ptr<PooledConnection> connection)
    {
        if (isCancelled(co_await asio::this_coro::cancellation_state))
            co_return LeaseResult{std::unexpected(cancelled("The wait for a connection"))};
        reserve(connection);
        connection->checking = true;
        // The end of an idle connection may still be on its way to the pool, its strand knows already.
        const auto control = connection->control;
        auto [exception, failure] = co_await asio::co_spawn(
            control->strand(),
            [control]() -> asio::awaitable<std::optional<RemoteIo::RemoteError>>
            {
                if (control->state() == ControlState::Open)
                    co_return std::nullopt;
                co_return control->failure().value_or(
                    errorOf(RemoteErrorKind::ConnectionLost, "The connection was closed")
                );
            },
            useTuple
        );
        connection->checking = false;
        const auto open = !exception && !failure;
        const auto cancelledCheck = isCancelled(co_await asio::this_coro::cancellation_state);
        if (open && !cancelledCheck)
            co_return LeaseResult{Lease{weak_from_this(), std::move(connection)}};
        if (open)
        {
            release(connection, true);
            co_return LeaseResult{std::unexpected(cancelled("The wait for a connection"))};
        }
        // It ended while idle.
        connection->leased = false;
        lose(
            connection, failure.value_or(errorOf(RemoteErrorKind::Failure, "The check of a connection failed")), false
        );
        if (cancelledCheck)
            co_return LeaseResult{std::unexpected(cancelled("The wait for a connection"))};
        co_return std::nullopt;
    }

    asio::awaitable<Pool::LeaseResult> Pool::waitForMetadata()
    {
        const auto waiter = std::make_shared<Waiter>(strand_);
        if (health_ == PoolHealth::Degraded)
            waiter->deadline = Clock::now() + options_.control.connectTimeout;
        commandWaiters_.push_back(waiter);
        auto cancelledWait = isCancelled(co_await asio::this_coro::cancellation_state);
        auto expired = false;
        while (!waiter->outcome && !cancelledWait && !expired)
        {
            waiter->timer.expires_at(waiter->deadline.value_or(Clock::time_point::max()));
            co_await waiter->timer.async_wait(useTuple);
            cancelledWait = isCancelled(co_await asio::this_coro::cancellation_state);
            expired = health_ == PoolHealth::Degraded && waiter->deadline && Clock::now() >= *waiter->deadline;
        }
        if (!waiter->outcome)
        {
            std::erase(commandWaiters_, waiter);
            if (cancelledWait)
                co_return std::unexpected(cancelled("The wait for the metadata connection"));
            co_return std::unexpected(
                errorOf(RemoteErrorKind::TimedOut, "The metadata connection did not log in again in time")
            );
        }
        auto outcome = std::move(*waiter->outcome);
        if (!outcome)
            co_return std::unexpected(std::move(outcome.error()));
        if (cancelledWait)
        {
            // Nothing went out on it, the next in turn takes it.
            release(*outcome, true);
            co_return std::unexpected(cancelled("The wait for the metadata connection"));
        }
        co_return Lease{weak_from_this(), std::move(*outcome)};
    }

    asio::awaitable<void> Pool::logInMetadataOnce()
    {
        auto connection = co_await logInConnection(true);
        metadataConnecting_ = false;
        if (connection && (closed_ || health_ == PoolHealth::Lost))
        {
            (*connection)->control->close();
            co_return;
        }
        if (connection)
        {
            adoptMetadata(std::move(*connection));
            co_return;
        }
        if (closed_)
            co_return;
        Log::info("FTP: Logging in C0 again failed: {}", RemoteIo::toString(connection.error().kind));
        failCommandWaiters(connection.error());
        if (worthAnotherLogin(connection.error()))
            degrade(connection.error());
        else
            loseSession(connection.error());
    }

    asio::awaitable<void> Pool::logInMetadataInBackground(RemoteIo::RemoteError failure)
    {
        auto backoff = options_.reloginBackoff;
        for (std::size_t attempt = 0u; attempt < options_.reloginAttempts && !closed_; ++attempt)
        {
            if (attempt > 0u)
            {
                reloginTimer_.expires_after(backoff);
                co_await reloginTimer_.async_wait(useTuple);
                backoff *= 2;
            }
            if (closed_ || health_ != PoolHealth::Degraded)
                co_return;
            if (const auto connection = idleTransfer())
            {
                promote(connection);
                co_return;
            }
            auto connection = co_await logInConnection(true);
            if (connection && (closed_ || health_ != PoolHealth::Degraded))
            {
                (*connection)->control->close();
                co_return;
            }
            if (connection)
            {
                adoptMetadata(std::move(*connection));
                co_return;
            }
            failure = std::move(connection.error());
            if (failure.kind == RemoteErrorKind::ConnectionLimit && !transfers_.empty())
            {
                // One may have become free during the login.
                if (const auto idleConnection = idleTransfer())
                {
                    promote(idleConnection);
                    co_return;
                }
                Log::info("FTP: The session's transfer connections hold every slot, the next one free becomes C0");
                metadataFromNextRelease_ = true;
                co_return;
            }
            if (!worthAnotherLogin(failure))
                break;
        }
        if (!closed_)
            loseSession(std::move(failure));
    }

    asio::awaitable<void> Pool::sendNoop(std::shared_ptr<PooledConnection> connection)
    {
        // A connection that ended reports through the release.
        const Lease held{weak_from_this(), connection};
        const auto control = connection->control;
        co_await asio::co_spawn(
            control->strand(), withoutThrowing(control->execute(Command{.verb = Verb::Noop})), useTuple
        );
    }

    asio::awaitable<void> Pool::quitAll(std::vector<std::shared_ptr<PooledConnection>> connections)
    {
        if (connections.empty())
            co_return;
        struct Countdown
        {
            Countdown(asio::any_io_executor executor, std::size_t count)
                : done{std::move(executor), Clock::time_point::max()}
                , remaining{count}
            {}

            asio::steady_timer done;
            std::size_t remaining;
        };
        const auto countdown = std::make_shared<Countdown>(strand_, connections.size());
        for (auto const& connection : connections)
        {
            asio::co_spawn(
                strand_,
                withoutThrowing(quit(connection->control, options_.quitTimeout)),
                asio::bind_executor(
                    strand_,
                    [countdown](std::exception_ptr)
                    {
                        if (--countdown->remaining == 0u)
                            countdown->done.cancel();
                    }
                )
            );
        }
        co_await countdown->done.async_wait(useTuple);
    }

    void Pool::spawn(asio::awaitable<void> work)
    {
        asio::co_spawn(
            strand_,
            withoutThrowing(std::move(work)),
            asio::bind_executor(
                strand_,
                [self = shared_from_this()](std::exception_ptr exception)
                {
                    if (!exception)
                        return;
                    // The work may have ended halfway, nothing else brings the pool back to a state it can go on from.
                    const auto what = Detail::describe(exception);
                    Log::error("FTP: Work of the connection pool ended with an exception, the session is lost: {}", what);
                    if (!self->closed_)
                        self->loseSession(errorOf(
                            RemoteErrorKind::ConnectionLost,
                            fmt::format("Work of the connection pool ended with an exception: {}", what)
                        ));
                }
            )
        );
    }

    std::optional<RemoteIo::RemoteError> Pool::refusal() const
    {
        if (closed_)
            return closing();
        if (health_ == PoolHealth::Lost)
        {
            assert(lostReason_ && "a lost session knows why");
            return errorOf(
                RemoteErrorKind::ConnectionLost,
                fmt::format("The FTP session lost its connection: {}", lostReason_->message)
            );
        }
        return std::nullopt;
    }

    void Pool::reserve(std::shared_ptr<PooledConnection> const& connection)
    {
        connection->leased = true;
        ++connection->idleGeneration;
        connection->idleTimer.cancel();
    }

    Lease Pool::lease(std::shared_ptr<PooledConnection> const& connection)
    {
        reserve(connection);
        return Lease{weak_from_this(), connection};
    }

    std::shared_ptr<PooledConnection> Pool::idleTransfer() const
    {
        const auto found = std::ranges::find_if(
            transfers_,
            [](auto const& connection)
            {
                return !connection->leased;
            }
        );
        if (found == transfers_.end())
            return nullptr;
        return *found;
    }

    bool Pool::metadataFree() const
    {
        return metadata_ && !metadata_->leased && commandWaiters_.empty();
    }

    std::size_t Pool::transferCapacity()
    {
        if (limitedUntil_ && Clock::now() >= *limitedUntil_)
        {
            limitedUntil_.reset();
            connectionLimit_ = options_.transferConnections + 1u;
        }
        return connectionLimit_ - 1u;
    }

    void Pool::learnLimit()
    {
        // C0 keeps its slot, also while it is not logged in.
        const auto limit = transfers_.size() + 1u;
        Log::info("FTP: The server refused another connection, the session keeps to {} for now", limit);
        connectionLimit_ = limit;
        limitedUntil_ = Clock::now() + options_.connectionLimitBackoff;
        limitTimer_.expires_at(*limitedUntil_);
        limitTimer_.async_wait(
            [weakPool = weak_from_this()](boost::system::error_code error)
            {
                const auto pool = weakPool.lock();
                if (error || !pool)
                    return;
                pool->endLimit();
            }
        );
    }

    void Pool::endLimit()
    {
        // A later refusal lowered the limit anew, its own wait follows.
        if (closed_ || (limitedUntil_ && Clock::now() < *limitedUntil_))
            return;
        transferCapacity();
        fireWakes();
    }

    void Pool::connectMetadataIfGone()
    {
        if (metadata_ || metadataConnecting_ || health_ != PoolHealth::Healthy)
            return;
        metadataConnecting_ = true;
        spawn(logInMetadataOnce());
    }

    void Pool::adoptMetadata(std::shared_ptr<PooledConnection> connection)
    {
        if (health_ == PoolHealth::Degraded)
            Log::info("FTP: {} serves as C0 again", connection->name);
        metadata_ = std::move(connection);
        health_ = PoolHealth::Healthy;
        for (auto const& waiter : commandWaiters_)
            waiter->deadline.reset();
        makeAvailable(metadata_);
    }

    void Pool::promote(std::shared_ptr<PooledConnection> const& connection)
    {
        std::erase(transfers_, connection);
        adoptMetadata(connection);
        // Its place among the transfer connections is free.
        fireWakes();
    }

    bool Pool::grantMetadata()
    {
        if (!metadata_ || metadata_->leased || commandWaiters_.empty())
            return false;
        const auto waiter = commandWaiters_.front();
        commandWaiters_.pop_front();
        reserve(metadata_);
        waiter->outcome = metadata_;
        waiter->timer.cancel();
        return true;
    }

    void Pool::failCommandWaiters(RemoteIo::RemoteError const& error)
    {
        for (auto const& waiter : std::exchange(commandWaiters_, {}))
        {
            waiter->outcome = std::unexpected(error);
            waiter->timer.cancel();
        }
    }

    void Pool::fireWakes()
    {
        // A wake may be what lets go of the pool.
        const auto wakes = std::exchange(wakes_, {});
        for (auto wake = wakes.begin(); wake != wakes.end() && !abandoned_; ++wake)
            (*wake)();
    }

    void Pool::abandon()
    {
        abandoned_ = true;
        closed_ = true;
        wakes_.clear();
        reloginTimer_.cancel();
        limitTimer_.cancel();
        failCommandWaiters(closing());
        closeConnections();
    }

    void Pool::closeConnections()
    {
        for (auto const& connection : connecting_)
            connection->control->close();
        for (auto const& connection : transfers_)
            connection->control->close();
        if (metadata_)
            metadata_->control->close();
    }

    void Pool::degrade(RemoteIo::RemoteError const& error)
    {
        if (health_ != PoolHealth::Healthy || closed_)
            return;
        Log::warn("FTP: Lost C0 during use, logging in again in the background: {}", RemoteIo::toString(error.kind));
        health_ = PoolHealth::Degraded;
        const auto deadline = Clock::now() + options_.control.connectTimeout;
        for (auto const& waiter : commandWaiters_)
        {
            waiter->deadline = deadline;
            waiter->timer.cancel();
        }
        spawn(logInMetadataInBackground(error));
    }

    void Pool::loseSession(RemoteIo::RemoteError error)
    {
        if (health_ == PoolHealth::Lost)
            return;
        Log::warn("FTP: The session lost its connection: {}", RemoteIo::toString(error.kind));
        lostReason_ = std::move(error);
        health_ = PoolHealth::Lost;
        failCommandWaiters(*refusal());
        fireWakes();
        if (onLost_ && !abandoned_)
            onLost_(*lostReason_);
    }

    void Pool::release(std::shared_ptr<PooledConnection> const& connection, bool open)
    {
        connection->leased = false;
        if (connection->dropped)
            return;
        if (closed_)
        {
            drop(connection);
            return;
        }
        if (!open)
        {
            lose(
                connection,
                errorOf(RemoteErrorKind::ConnectionLost, fmt::format("{} ended during use", connection->name)),
                true
            );
            return;
        }
        if (connection != metadata_ && metadataFromNextRelease_)
        {
            metadataFromNextRelease_ = false;
            promote(connection);
            return;
        }
        makeAvailable(connection);
    }

    void Pool::takeEnd(std::shared_ptr<PooledConnection> const& connection, RemoteIo::RemoteError const& error)
    {
        if (connection->dropped || closed_)
            return;
        lose(connection, error, connection->leased && !connection->checking);
    }

    void Pool::lose(std::shared_ptr<PooledConnection> const& connection, RemoteIo::RemoteError const& error, bool inUse)
    {
        const auto metadata = connection == metadata_;
        const auto transfer = std::ranges::find(transfers_, connection) != transfers_.end();
        // A connection whose login runs reports its end through the login.
        if (!metadata && !transfer)
            return;
        Log::debug("FTP: {} ended: {}", connection->name, RemoteIo::toString(error.kind));
        drop(connection);
        if (metadata && inUse)
            degrade(error);
        else if (metadata && !commandWaiters_.empty())
        {
            // The one whose check found the idle C0 ended may be gone, the commands behind it still wait for C0.
            connectMetadataIfGone();
        }
        if (transfer && metadataFromNextRelease_)
        {
            // A slot is free, C0 may log in.
            metadataFromNextRelease_ = false;
            spawn(logInMetadataInBackground(error));
        }
        fireWakes();
    }

    void Pool::drop(std::shared_ptr<PooledConnection> const& connection)
    {
        connection->dropped = true;
        ++connection->idleGeneration;
        connection->idleTimer.cancel();
        connection->control->close();
        if (connection == metadata_)
            metadata_.reset();
        std::erase(transfers_, connection);
    }

    void Pool::makeAvailable(std::shared_ptr<PooledConnection> const& connection)
    {
        connection->leased = false;
        if (connection == metadata_)
        {
            if (grantMetadata())
                return;
            if (options_.keepIdleAlive)
                armIdleTimer(connection, options_.idleNoopInterval);
            if (transferCapacity() == 0u)
                fireWakes();
            return;
        }
        armIdleTimer(connection, options_.transferLinger);
        fireWakes();
    }

    void Pool::armIdleTimer(std::shared_ptr<PooledConnection> const& connection, std::chrono::milliseconds duration)
    {
        const auto generation = ++connection->idleGeneration;
        connection->idleTimer.expires_after(duration);
        connection->idleTimer.async_wait(
            [weakPool = weak_from_this(),
                weakConnection = std::weak_ptr{connection},
                generation](boost::system::error_code error)
            {
                const auto pool = weakPool.lock();
                const auto connection = weakConnection.lock();
                if (error || !pool || !connection || connection->idleGeneration != generation)
                    return;
                pool->endIdleTime(connection);
            }
        );
    }

    void Pool::endIdleTime(std::shared_ptr<PooledConnection> const& connection)
    {
        if (connection->leased || connection->dropped || closed_)
            return;
        if (connection == metadata_)
        {
            reserve(connection);
            spawn(sendNoop(connection));
            return;
        }
        Log::debug("FTP: {} was idle for its linger time", connection->name);
        connection->dropped = true;
        std::erase(transfers_, connection);
        spawn(quit(connection->control, options_.quitTimeout));
    }
}
