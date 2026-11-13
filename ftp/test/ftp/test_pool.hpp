#pragma once

#include "test_control_connection_tls.hpp"

#include <ftp/engine/resolver.hpp>
#include <ftp/pool/pool.hpp>
#include <ftp/support/scripted_ftp_server.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace FileTransferProtocol::Test
{
    namespace
    {
        namespace asio = boost::asio;
        using namespace std::chrono_literals;
        using RemoteIo::RemoteErrorKind;

        /**
         * @brief FEAT, OPTS UTF8 ON, TYPE I and PWD answered like vsftpd does.
         */
        Script& afterLogin(Script& script)
        {
            return script.featLike(ServerLike::Vsftpd)
                .expect("OPTS", "UTF8 ON")
                .reply("200 Always in UTF8 mode.")
                .expect("TYPE", "I")
                .reply("200 Switching to Binary mode.")
                .expect("PWD")
                .reply("257 \"/home/user\" is the current directory");
        }

        /**
         * @brief The whole login: Script::login(), then afterLogin().
         */
        Script& loggedIn(Script& script)
        {
            return afterLogin(script.login());
        }

        /**
         * @brief A connection that logs in and ends when the client closes it.
         */
        Script loginThenDisconnect()
        {
            Script script{};
            return loggedIn(script).expectDisconnect();
        }

        /**
         * @brief A connection that logs in, then ends with the reset @p verb gets: lost during use.
         */
        Script loginThenResetOn(std::string verb)
        {
            Script script{};
            return loggedIn(script).expect(std::move(verb)).resetControl();
        }

        /**
         * @brief A connection refused at the greeting the way vsftpd refuses one over its limit.
         */
        Script refusedAtTheGreeting()
        {
            Script script{};
            return script.reply("421 There are too many connections from your internet address.").expectDisconnect();
        }

        /**
         * @brief Counts the calls of a Pool::Wake and tells of the first. The wake owns it, the pool may call a wake it
         *        kept after the test.
         */
        struct WakeCount : std::enable_shared_from_this<WakeCount>
        {
            Pool::Wake wake()
            {
                return [self = shared_from_this()]()
                {
                    if (++self->calls == 1u)
                        self->first.set_value();
                };
            }

            std::atomic<std::size_t> calls{0u};
            std::promise<void> first{};
            std::future<void> woken{first.get_future()};
        };

        /**
         * @brief Keeps a strand busy until it goes out of scope, what is queued meanwhile runs after it in order.
         */
        class StrandHold
        {
          public:
            explicit StrandHold(asio::strand<asio::any_io_executor> const& strand)
            {
                auto entered = std::make_shared<std::promise<void>>();
                auto running = entered->get_future();
                asio::post(
                    strand,
                    [entered, left = left_]()
                    {
                        entered->set_value();
                        left.wait();
                    }
                );
                if (running.wait_for(10s) != std::future_status::ready)
                    throw std::runtime_error{"The strand did not run the hold"};
            }

            ~StrandHold()
            {
                leave_.set_value();
            }

            StrandHold(StrandHold const&) = delete;
            StrandHold& operator=(StrandHold const&) = delete;
            StrandHold(StrandHold&&) = delete;
            StrandHold& operator=(StrandHold&&) = delete;

          private:
            std::promise<void> leave_{};
            std::shared_future<void> left_{leave_.get_future().share()};
        };

        /**
         * @brief Asks @p condition until it holds, for a state the pool shows by no event.
         *
         * @return False when it did not hold within ten seconds.
         */
        template <typename ConditionT>
        bool eventually(ConditionT condition)
        {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            auto holds = condition();
            while (!holds && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(1ms);
                holds = condition();
            }
            return holds;
        }

        /**
         * @brief Tells when its last owner let go. A pool's lost handler that holds it tells when the pool is gone.
         */
        struct EndSignal
        {
            EndSignal() = default;
            ~EndSignal()
            {
                ended.set_value();
            }
            EndSignal(EndSignal const&) = delete;
            EndSignal& operator=(EndSignal const&) = delete;
            EndSignal(EndSignal&&) = delete;
            EndSignal& operator=(EndSignal&&) = delete;

            std::promise<void> ended{};
        };

        /**
         * @brief What the lost handler of a pool got.
         */
        struct Losses
        {
            void record(RemoteIo::RemoteError const& error)
            {
                std::scoped_lock lock{mutex};
                errors.push_back(error);
                if (errors.size() == 1u)
                    first.set_value();
            }

            std::vector<RemoteIo::RemoteError> recorded()
            {
                std::scoped_lock lock{mutex};
                return errors;
            }

            std::mutex mutex{};
            std::vector<RemoteIo::RemoteError> errors{};
            std::promise<void> first{};
            std::future<void> lost{first.get_future()};
        };
    }

    class PoolTests : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto started = ScriptedFtpServer::start();
            ASSERT_TRUE(started) << started.error();
            server_ = std::move(started).value();
            setEndpoints({endpointOf(*server_)});
        }

        void TearDown() override
        {
            // The destructor closes the connections without QUIT, close() has a test of its own.
            pool_.reset();
            if (!server_->waitUntilDisconnected())
            {
                std::string transcript{};
                for (auto const& entry : server_->transcript())
                    transcript +=
                        fmt::format("\n  {} {}: {}", entry.connection, static_cast<int>(entry.kind), entry.detail);
                ADD_FAILURE() << "The server still has a connection open, its transcript:" << transcript;
            }
            for (auto const& failure : server_->failures())
                ADD_FAILURE() << failure;
        }

        static asio::ip::tcp::endpoint endpointOf(ScriptedFtpServer const& server)
        {
            return {asio::ip::make_address(server.address()), server.port()};
        }

        /**
         * @brief What the lookups find from now on, none makes them fail.
         */
        void setEndpoints(Endpoints endpoints)
        {
            std::scoped_lock lock{lookupMutex_};
            endpoints_ = std::move(endpoints);
        }

        static PoolOptions options()
        {
            return PoolOptions{
                .host = "ftp.test",
                .login = {.credentials = {.user = "user", .password = "password"}},
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
                .transferConnections = 2u,
            };
        }

        std::shared_ptr<Pool> makePool(PoolOptions poolOptions)
        {
            pool_ = Pool::create(
                asio::make_strand(threads_.get_executor()),
                resolver_,
                std::move(poolOptions),
                [losses = losses_](RemoteIo::RemoteError const& error)
                {
                    losses->record(error);
                }
            );
            return pool_;
        }

        /**
         * @brief Runs @p work, an awaitable or a function that returns one, on the pool's strand and waits for it.
         */
        template <typename WorkT>
        auto run(WorkT work)
        {
            auto future = asio::co_spawn(pool_->strand(), std::move(work), asio::use_future);
            if (future.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The coroutine did not end"};
            return future.get();
        }

        /**
         * @brief A pool whose start logged in C0.
         */
        std::shared_ptr<Pool> startedPool(PoolOptions poolOptions = options())
        {
            auto pool = makePool(std::move(poolOptions));
            const auto login = run(pool->start());
            if (!login)
                throw std::runtime_error{fmt::format("The start failed: {}", login.error().toString())};
            return pool;
        }

        template <typename AwaitableT>
        auto spawn(AwaitableT awaitable, asio::cancellation_slot slot = {})
        {
            return asio::co_spawn(
                pool_->strand(), std::move(awaitable), asio::bind_cancellation_slot(slot, asio::use_future)
            );
        }

        template <typename ValueT>
        static ValueT waitFor(std::future<ValueT>& future)
        {
            if (future.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The wait did not end"};
            return future.get();
        }

        /**
         * @brief Waits for what is queued on @p strand now, a coroutine spawned there before reached its first wait.
         */
        static void flush(asio::strand<asio::any_io_executor> const& strand)
        {
            auto future = asio::co_spawn(
                strand,
                []() -> asio::awaitable<void>
                {
                    co_return;
                },
                asio::use_future
            );
            if (future.wait_for(20s) != std::future_status::ready)
                throw std::runtime_error{"The strand did not run"};
        }

        static Lease leaseFrom(Pool::LeaseResult result)
        {
            if (!result)
                throw std::runtime_error{fmt::format("No lease: {}", result.error().toString())};
            return std::move(*result);
        }

        /**
         * @brief Releases @p lease and waits until the pool took the connection back.
         */
        void giveBack(Lease& lease)
        {
            const auto strand = lease.control()->strand();
            lease.release();
            flush(strand);
            flush(pool_->strand());
        }

        static std::expected<Reply, RemoteIo::RemoteError> execute(Lease const& lease, Verb verb)
        {
            const auto control = lease.control();
            auto future = asio::co_spawn(control->strand(), control->execute(Command{.verb = verb}), asio::use_future);
            return waitFor(future);
        }

        std::size_t connectionsAccepted() const
        {
            const auto transcript = server_->transcript();
            return static_cast<std::size_t>(std::ranges::count_if(
                transcript,
                [](TranscriptEntry const& entry)
                {
                    return entry.kind == TranscriptEntry::Kind::Connected;
                }
            ));
        }

        static void expectError(Pool::LeaseResult const& result, RemoteErrorKind kind)
        {
            ASSERT_FALSE(result) << "got a lease of " << result->name();
            EXPECT_EQ(result.error().kind, kind) << result.error().toString();
        }

        /**
         * @brief Ends C0 while it is idle with the 421 of a server's idle timeout, and waits until the client closed
         *        it, which it does on the connection's strand once it took the 421.
         */
        void endIdleMetadata()
        {
            server_->release("idle");
            ASSERT_TRUE(server_->waitForTranscriptEntry(TranscriptEntry::Kind::Disconnected));
        }

        /**
         * @brief A C0 that the server ends with an idle timeout once the test calls endIdleMetadata().
         */
        static Script idleTimeoutAfterLogin()
        {
            Script script{};
            return loggedIn(script).awaitRelease("idle").unsolicited("421 Timeout.").expectDisconnect();
        }

      protected:
        asio::thread_pool threads_{2};
        std::unique_ptr<ScriptedFtpServer> server_{};
        std::mutex lookupMutex_{};
        Endpoints endpoints_{};
        std::atomic<std::size_t> lookups_{0u};
        std::shared_ptr<Resolver> resolver_ = std::make_shared<Resolver>(
            ResolverOptions{},
            [this](std::string const&, std::uint16_t) -> ResolveResult
            {
                ++lookups_;
                std::scoped_lock lock{lookupMutex_};
                if (endpoints_.empty())
                    return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteErrorKind::NoConnection,
                            .message = "Could not look up the server: no such host"
                        }
                    );
                return endpoints_;
            }
        );
        std::shared_ptr<Losses> losses_ = std::make_shared<Losses>();
        std::shared_ptr<Pool> pool_{};
    };

    TEST_F(PoolTests, StartLooksUpTheServerAndLogsInC0)
    {
        server_->addConnection(loginThenDisconnect());
        const auto pool = makePool(options());
        const auto login = run(pool->start());
        ASSERT_TRUE(login) << login.error().toString();
        EXPECT_EQ(login->homeDirectory, "/home/user");
        EXPECT_EQ(lookups_, 1u);
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_EQ(pool->connectionLimit(), 3u);
    }

    TEST_F(PoolTests, FailedLookupFailsTheStartWithoutALoss)
    {
        setEndpoints({});
        const auto pool = makePool(options());
        const auto login = run(pool->start());
        ASSERT_FALSE(login) << "logged in without an address";
        EXPECT_EQ(login.error().kind, RemoteErrorKind::NoConnection) << login.error().toString();
        EXPECT_EQ(pool->health(), PoolHealth::Lost);
        EXPECT_TRUE(losses_->recorded().empty());
        expectError(run(pool->acquireCommand()), RemoteErrorKind::ConnectionLost);
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::ConnectionLost);
    }

    TEST_F(PoolTests, CloseDuringTheLookupOfTheStartLogsInNothing)
    {
        const auto pool = makePool(options());
        std::future<std::expected<LoginResult, RemoteIo::RemoteError>> started{};
        {
            // The lookup waits for this lock, so the close comes before its answer.
            const std::scoped_lock held{lookupMutex_};
            started = spawn(pool->start());
            run(pool->close());
        }
        const auto login = waitFor(started);
        ASSERT_FALSE(login) << "the start logged in after the close";
        EXPECT_EQ(login.error().kind, RemoteErrorKind::ConnectionLost) << login.error().toString();
        EXPECT_EQ(connectionsAccepted(), 0u);
        EXPECT_TRUE(losses_->recorded().empty());
    }

    TEST_F(PoolTests, UntrustedCertificateAtTheStartIsKeptForThePrompt)
    {
        Script script{};
        script.reply("220 Ready")
            .expect("AUTH", "TLS")
            .reply("234 Proceed")
            .startTls({.certificate = tlsTestPki().selfSigned})
            .expectDisconnect();
        server_->addConnection(std::move(script));
        auto context = TlsContext::create({.systemAuthorities = false, .authorities = {tlsTestPki().authority}});
        ASSERT_TRUE(context) << context.error().toString();
        auto poolOptions = options();
        poolOptions.login.tlsMode = TlsMode::Explicit;
        poolOptions.login.tls = TlsConnectionSettings{.context = std::move(context).value(), .serverName = "127.0.0.1"};

        const auto pool = makePool(std::move(poolOptions));
        const auto login = run(pool->start());
        ASSERT_FALSE(login) << "trusted a self-signed certificate";
        EXPECT_EQ(login.error().kind, RemoteErrorKind::TrustRequired) << login.error().toString();
        const auto untrusted = run(
            [pool]() -> asio::awaitable<std::optional<CertificateInfo>>
            {
                co_return pool->untrustedCertificate();
            }
        );
        ASSERT_TRUE(untrusted);
        EXPECT_EQ(untrusted->publicKeySha256, publicKeyDigestOf(tlsTestPki().selfSigned));
    }

    TEST_F(PoolTests, CommandsTakeC0InTurn)
    {
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto first = leaseFrom(run(pool->acquireCommand()));
        EXPECT_EQ(first.name(), "C0");
        auto second = spawn(pool->acquireCommand());
        flush(pool->strand());
        EXPECT_NE(second.wait_for(0s), std::future_status::ready) << "a second command got C0 while the first held it";

        first.release();
        const auto lease = leaseFrom(waitFor(second));
        EXPECT_EQ(lease.name(), "C0");
        EXPECT_EQ(connectionsAccepted(), 1u);
    }

    TEST_F(PoolTests, CommandTakesAnIdleTransferConnectionRatherThanWaitForC0)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(transfer.name(), "C1");
        giveBack(transfer);

        const auto first = leaseFrom(run(pool->acquireCommand()));
        const auto second = leaseFrom(run(pool->acquireCommand()));
        EXPECT_EQ(first.name(), "C0");
        EXPECT_EQ(second.name(), "C1");
    }

    TEST_F(PoolTests, TransferConnectionLogsInOnceAndServesTheNextTransfer)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto first = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(first.name(), "C1");
        EXPECT_FALSE(first.transferSession().tls);
        EXPECT_TRUE(first.transferSession().restartStream);
        giveBack(first);

        const auto second = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(second.name(), "C1");
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, TransfersBeyondTheirConnectionsAreBusyAndWokenOnARelease)
    {
        for (auto index = 0; index < 3; ++index)
            server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto first = leaseFrom(run(pool->acquireTransfer()));
        const auto second = leaseFrom(run(pool->acquireTransfer()));
        const auto wakes = std::make_shared<WakeCount>();
        expectError(run(pool->acquireTransfer(wakes->wake())), RemoteErrorKind::Busy);
        EXPECT_EQ(connectionsAccepted(), 3u);
        EXPECT_EQ(wakes->calls, 0u);

        giveBack(first);
        ASSERT_EQ(wakes->woken.wait_for(10s), std::future_status::ready);
        EXPECT_EQ(wakes->calls, 1u);
        const auto again = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(again.name(), "C1");
    }

    TEST_F(PoolTests, ConnectionLimitAtTheGreetingLowersTheLimit)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        auto poolOptions = options();
        poolOptions.transferConnections = 3u;
        const auto pool = startedPool(std::move(poolOptions));
        const auto first = leaseFrom(run(pool->acquireTransfer()));
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::Busy);
        EXPECT_EQ(pool->connectionLimit(), 2u);
        // Within the backoff no further login is tried.
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::Busy);
        EXPECT_EQ(connectionsAccepted(), 3u);
        EXPECT_EQ(lookups_, 1u);
    }

    TEST_F(PoolTests, ConnectionLimitAfterThePasswordLowersTheLimit)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        Script refused{};
        refused.reply("220 ProFTPD Server ready.")
            .expect("USER", "user")
            .reply("331 Password required for user")
            .expect("PASS", "password")
            .reply("530 Sorry, the maximum number of clients (3) from your host are already connected.")
            .expectDisconnect();
        server_->addConnection(std::move(refused));
        auto poolOptions = options();
        poolOptions.transferConnections = 3u;
        const auto pool = startedPool(std::move(poolOptions));
        const auto first = leaseFrom(run(pool->acquireTransfer()));
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::Busy);
        EXPECT_EQ(pool->connectionLimit(), 2u);
    }

    TEST_F(PoolTests, LoginUnderWayAtARefusalCountsToTheLimitOnceItGotThrough)
    {
        server_->addConnection(loginThenDisconnect());
        Script slow{};
        slow.checkpoint("accepted");
        server_->addConnection(afterLogin(slow.login().awaitRelease("login")).expectDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        const auto pool = startedPool();
        auto underWay = spawn(pool->acquireTransfer());
        ASSERT_TRUE(server_->waitForCheckpoint("accepted"));

        // The refusal counts the connections the session has, the login under way is none of them yet.
        const auto refused = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(refused.name(), "C0");
        EXPECT_EQ(pool->connectionLimit(), 1u);

        server_->release("login");
        EXPECT_EQ(leaseFrom(waitFor(underWay)).name(), "C1");
        EXPECT_EQ(pool->connectionLimit(), 2u);
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, LimitIsTriedAgainAfterItsBackoff)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        server_->addConnection(loginThenDisconnect());
        auto poolOptions = options();
        poolOptions.transferConnections = 3u;
        poolOptions.connectionLimitBackoff = 0ms;
        const auto pool = startedPool(std::move(poolOptions));
        const auto first = leaseFrom(run(pool->acquireTransfer()));
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::Busy);

        const auto retried = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(retried.name(), "C3");
        EXPECT_EQ(connectionsAccepted(), 4u);
    }

    TEST_F(PoolTests, WaitingTransferIsWokenOnceTheLimitMayBeTriedAgain)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        server_->addConnection(loginThenDisconnect());
        auto poolOptions = options();
        poolOptions.connectionLimitBackoff = 300ms;
        const auto pool = startedPool(std::move(poolOptions));
        const auto wakes = std::make_shared<WakeCount>();
        // Both within the time of the limit, nothing but the pool's strand runs between them.
        auto [held, waiting] = run(
            [&pool, &wakes]() -> asio::awaitable<std::pair<Pool::LeaseResult, Pool::LeaseResult>>
            {
                auto first = co_await pool->acquireTransfer();
                auto second = co_await pool->acquireTransfer(wakes->wake());
                co_return std::pair{std::move(first), std::move(second)};
            }
        );
        const auto transfer = leaseFrom(std::move(held));
        EXPECT_EQ(transfer.name(), "C0");
        expectError(waiting, RemoteErrorKind::Busy);

        // C0 stays in use, only the end of the limit wakes the transfer.
        ASSERT_EQ(wakes->woken.wait_for(10s), std::future_status::ready);
        EXPECT_EQ(pool->connectionLimit(), 3u);
        EXPECT_EQ(leaseFrom(run(pool->acquireTransfer())).name(), "C2");
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, TransfersTakeC0WhenTheLimitLeavesOnlyC0)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        const auto pool = startedPool();
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(transfer.name(), "C0");
        EXPECT_EQ(pool->connectionLimit(), 1u);

        auto command = spawn(pool->acquireCommand());
        flush(pool->strand());
        EXPECT_NE(command.wait_for(0s), std::future_status::ready) << "a command got C0 during its transfer";
        const auto wakes = std::make_shared<WakeCount>();
        expectError(run(pool->acquireTransfer(wakes->wake())), RemoteErrorKind::Busy);
        giveBack(transfer);
        auto granted = leaseFrom(waitFor(command));
        EXPECT_EQ(granted.name(), "C0");
        EXPECT_EQ(wakes->calls, 0u) << "the transfer was woken while the command that waited has C0";

        giveBack(granted);
        ASSERT_EQ(wakes->woken.wait_for(10s), std::future_status::ready);
        EXPECT_EQ(leaseFrom(run(pool->acquireTransfer())).name(), "C0");
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, ACommandBesideTheOnlyConnectionItsCallerHoldsIsBusyAtOnce)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        const auto pool = startedPool();
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        ASSERT_EQ(transfer.name(), "C0");

        // Only the caller could end a wait for C0.
        expectError(run(pool->acquireCommand(transfer.control())), RemoteErrorKind::Busy);
        giveBack(transfer);
        EXPECT_EQ(leaseFrom(run(pool->acquireCommand())).name(), "C0");
    }

    TEST_F(PoolTests, C0ThatEndedWhileIdleLogsInAtItsNextUse)
    {
        server_->addConnection(idleTimeoutAfterLogin());
        Script again{};
        server_->addConnection(loggedIn(again).expect("NOOP").reply("200 NOOP ok.").expectDisconnect());
        const auto pool = startedPool();
        ASSERT_NO_FATAL_FAILURE(endIdleMetadata());

        const auto lease = leaseFrom(run(pool->acquireCommand()));
        EXPECT_EQ(lease.name(), "C0");
        const auto reply = execute(lease, Verb::Noop);
        ASSERT_TRUE(reply) << reply.error().toString();
        EXPECT_EQ(reply->code, 200);
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_TRUE(losses_->recorded().empty());
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, C0LostDuringUseLogsInAgainInTheBackground)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto lease = leaseFrom(run(pool->acquireCommand()));
        const auto reply = execute(lease, Verb::Noop);
        ASSERT_FALSE(reply) << "the reset connection answered";
        auto waiting = spawn(pool->acquireCommand());

        giveBack(lease);
        const auto again = leaseFrom(waitFor(waiting));
        EXPECT_EQ(again.name(), "C0");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_TRUE(losses_->recorded().empty());
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, CommandsWhileDegradedWaitOnlyUpToTheConnectTimeout)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        Script slow{};
        server_->addConnection(afterLogin(slow.login().awaitRelease("login")).expectDisconnect());
        auto poolOptions = options();
        // The wait of the command ends by the clock alone.
        poolOptions.control.connectTimeout = 300ms;
        const auto pool = startedPool(std::move(poolOptions));
        auto lease = leaseFrom(run(pool->acquireCommand()));
        ASSERT_FALSE(execute(lease, Verb::Noop));
        giveBack(lease);

        expectError(run(pool->acquireCommand()), RemoteErrorKind::TimedOut);
        EXPECT_EQ(pool->health(), PoolHealth::Degraded);
        server_->release("login");
        const auto again = leaseFrom(run(pool->acquireCommand()));
        EXPECT_EQ(again.name(), "C0");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
    }

    TEST_F(PoolTests, BackgroundLoginGivesUpAndLosesTheSession)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        for (auto index = 0; index < 2; ++index)
        {
            Script refused{};
            server_->addConnection(
                refused.reply("421 Service not available, closing control connection.").expectDisconnect()
            );
        }
        auto poolOptions = options();
        poolOptions.reloginAttempts = 2u;
        poolOptions.reloginBackoff = 10ms;
        const auto pool = startedPool(std::move(poolOptions));
        auto lease = leaseFrom(run(pool->acquireCommand()));
        ASSERT_FALSE(execute(lease, Verb::Noop));
        giveBack(lease);

        ASSERT_EQ(losses_->lost.wait_for(10s), std::future_status::ready);
        const auto losses = losses_->recorded();
        ASSERT_EQ(losses.size(), 1u);
        EXPECT_EQ(losses.front().kind, RemoteErrorKind::NoConnection) << losses.front().toString();
        EXPECT_EQ(pool->health(), PoolHealth::Lost);
        expectError(run(pool->acquireCommand()), RemoteErrorKind::ConnectionLost);
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::ConnectionLost);
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, PoolWithoutItsOwnerEndsItsLoginsInTheBackground)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        Script refused{};
        server_->addConnection(refused.checkpoint("refused")
                .reply("421 Service not available, closing control connection.")
                .expectDisconnect());
        auto poolOptions = options();
        // Nothing but the end of the pool ends the wait for the next login.
        poolOptions.reloginBackoff = 60s;
        auto signal = std::make_shared<EndSignal>();
        auto poolEnded = signal->ended.get_future();
        pool_ = Pool::create(
            asio::make_strand(threads_.get_executor()),
            resolver_,
            std::move(poolOptions),
            [losses = losses_, signal = std::move(signal)](RemoteIo::RemoteError const& error)
            {
                losses->record(error);
            }
        );
        const auto login = run(pool_->start());
        ASSERT_TRUE(login) << login.error().toString();
        auto lease = leaseFrom(run(pool_->acquireCommand()));
        ASSERT_FALSE(execute(lease, Verb::Noop));
        giveBack(lease);
        ASSERT_TRUE(server_->waitForCheckpoint("refused"));

        pool_.reset();
        ASSERT_EQ(poolEnded.wait_for(10s), std::future_status::ready) << "the pool went on without its owner";
        EXPECT_TRUE(losses_->recorded().empty());
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, RefusedLoginInTheBackgroundLosesTheSessionAtOnce)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        Script refused{};
        refused.reply("220 Ready")
            .expect("USER", "user")
            .reply("331 Password required")
            .expect("PASS", "password")
            .reply("530 Login incorrect.")
            .expectDisconnect();
        server_->addConnection(std::move(refused));
        const auto pool = startedPool();
        auto lease = leaseFrom(run(pool->acquireCommand()));
        ASSERT_FALSE(execute(lease, Verb::Noop));
        giveBack(lease);

        ASSERT_EQ(losses_->lost.wait_for(10s), std::future_status::ready);
        const auto losses = losses_->recorded();
        ASSERT_EQ(losses.size(), 1u);
        EXPECT_EQ(losses.front().kind, RemoteErrorKind::AuthenticationFailed) << losses.front().toString();
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, FailedLoginOfAnIdleC0FailsTheCommandAndGoesOnInTheBackground)
    {
        server_->addConnection(idleTimeoutAfterLogin());
        Script refused{};
        server_->addConnection(refused.reply("421 Service not available.").expectDisconnect());
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        ASSERT_NO_FATAL_FAILURE(endIdleMetadata());

        expectError(run(pool->acquireCommand()), RemoteErrorKind::NoConnection);
        const auto again = leaseFrom(run(pool->acquireCommand()));
        EXPECT_EQ(again.name(), "C0");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_TRUE(losses_->recorded().empty());
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, ServerThatMovedIsFoundByANewLookup)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        auto old = std::move(started).value();
        Script stays{};
        old->addConnection(loggedIn(stays).stall());
        setEndpoints({endpointOf(*old)});
        Script moved{};
        server_->addConnection(loggedIn(moved).expect("NOOP").reply("200 NOOP ok.").expectDisconnect());
        const auto pool = startedPool();

        // C0 is in use when its server goes away, so the session logs in again in the background, where only a new
        // lookup finds the server.
        const auto held = leaseFrom(run(pool->acquireCommand()));
        setEndpoints({endpointOf(*server_)});
        EXPECT_TRUE(old->failures().empty());
        old.reset();

        const auto again = leaseFrom(run(pool->acquireCommand()));
        const auto reply = execute(again, Verb::Noop);
        ASSERT_TRUE(reply) << reply.error().toString();
        EXPECT_EQ(reply->code, 200);
        EXPECT_EQ(lookups_, 2u);
    }

    TEST_F(PoolTests, SameAddressesInAnotherOrderAreNoNewServer)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto other = std::move(started).value();
        setEndpoints({endpointOf(*server_), endpointOf(*other)});
        server_->addConnection(loginThenDisconnect());
        Script refused{};
        server_->addConnection(
            refused.reply("421 Service not available, closing control connection.").expectDisconnect()
        );
        const auto pool = startedPool();

        // What a DNS round robin answers the next time.
        setEndpoints({endpointOf(*other), endpointOf(*server_)});
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::NoConnection);
        EXPECT_EQ(lookups_, 2u);
        EXPECT_EQ(connectionsAccepted(), 2u);
        EXPECT_TRUE(other->transcript().empty()) << "the login was tried again at the same addresses";
    }

    TEST_F(PoolTests, IdleTransferConnectionEndsWithQuitAfterItsLinger)
    {
        server_->addConnection(loginThenDisconnect());
        Script lingering{};
        server_->addConnection(
            loggedIn(lingering).expect("QUIT").checkpoint("quit").reply("221 Goodbye.").expectDisconnect()
        );
        server_->addConnection(loginThenDisconnect());
        auto poolOptions = options();
        poolOptions.transferLinger = 50ms;
        const auto pool = startedPool(std::move(poolOptions));
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        giveBack(transfer);
        ASSERT_TRUE(server_->waitForCheckpoint("quit"));

        const auto next = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(next.name(), "C2");
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, KeepIdleAliveSendsNoopOnAnIdleC0)
    {
        Script script{};
        loggedIn(script)
            .expect("NOOP")
            .reply("200 NOOP ok.")
            .expect("NOOP")
            .checkpoint("second")
            .awaitRelease("answer")
            .reply("200 NOOP ok.")
            .expectDisconnect();
        server_->addConnection(std::move(script));
        auto poolOptions = options();
        poolOptions.keepIdleAlive = true;
        poolOptions.idleNoopInterval = 50ms;
        const auto pool = startedPool(std::move(poolOptions));
        ASSERT_TRUE(server_->waitForCheckpoint("second"));

        // Takes C0 from the second NOOP, so no third one goes out.
        auto command = spawn(pool->acquireCommand());
        flush(pool->strand());
        server_->release("answer");
        EXPECT_EQ(leaseFrom(waitFor(command)).name(), "C0");
    }

    TEST_F(PoolTests, ConnectionThatEndedDuringUseIsNotHandedOutAgain)
    {
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenResetOn("NOOP"));
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        ASSERT_FALSE(execute(transfer, Verb::Noop));
        giveBack(transfer);

        const auto next = leaseFrom(run(pool->acquireTransfer()));
        EXPECT_EQ(next.name(), "C2");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_EQ(connectionsAccepted(), 3u);
    }

    TEST_F(PoolTests, LeaseEndsOnAnyThread)
    {
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto lease = leaseFrom(run(pool->acquireCommand()));
        std::thread{
            [lease = std::move(lease)]() mutable
            {
                lease.release();
            }
        }.join();

        EXPECT_EQ(leaseFrom(run(pool->acquireCommand())).name(), "C0");
        EXPECT_EQ(connectionsAccepted(), 1u);
    }

    TEST_F(PoolTests, CancelledWaitForC0GivesUpItsTurn)
    {
        asio::cancellation_signal signal{};
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto first = leaseFrom(run(pool->acquireCommand()));
        auto cancelledWait = spawn(pool->acquireCommand(), signal.slot());
        auto nextWait = spawn(pool->acquireCommand());
        flush(pool->strand());
        asio::post(
            pool->strand(),
            [&signal]()
            {
                signal.emit(asio::cancellation_type::terminal);
            }
        );
        expectError(waitFor(cancelledWait), RemoteErrorKind::Cancelled);

        giveBack(first);
        EXPECT_EQ(leaseFrom(waitFor(nextWait)).name(), "C0");
    }

    TEST_F(PoolTests, CloseQuitsIdleConnectionsAndClosesTheOthers)
    {
        Script metadata{};
        server_->addConnection(loggedIn(metadata).expect("QUIT").reply("221 Goodbye.").expectDisconnect());
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        const auto transfer = leaseFrom(run(pool->acquireTransfer()));

        run(pool->close());
        ASSERT_TRUE(server_->waitUntilDisconnected());
        EXPECT_EQ(server_->commands(0u).back(), "QUIT");
        EXPECT_NE(server_->commands(1u).back(), "QUIT");
        expectError(run(pool->acquireCommand()), RemoteErrorKind::ConnectionLost);
        expectError(run(pool->acquireTransfer()), RemoteErrorKind::ConnectionLost);
    }

    TEST_F(PoolTests, CloseFailsTheWaitsForC0)
    {
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        const auto first = leaseFrom(run(pool->acquireCommand()));
        auto waiting = spawn(pool->acquireCommand());
        flush(pool->strand());

        run(pool->close());
        expectError(waitFor(waiting), RemoteErrorKind::ConnectionLost);
    }

    TEST_F(PoolTests, IdleTransferConnectionBecomesC0WithoutALogin)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        auto transfer = leaseFrom(run(pool->acquireTransfer()));
        giveBack(transfer);
        auto command = leaseFrom(run(pool->acquireCommand()));
        ASSERT_FALSE(execute(command, Verb::Noop));

        giveBack(command);
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_EQ(leaseFrom(run(pool->acquireCommand())).name(), "C1");
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, TransferConnectionBecomesC0WhenTheTransfersHoldEverySlot)
    {
        server_->addConnection(loginThenResetOn("NOOP"));
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(loginThenDisconnect());
        server_->addConnection(refusedAtTheGreeting());
        auto poolOptions = options();
        poolOptions.transferConnections = 3u;
        poolOptions.reloginBackoff = 10ms;
        const auto pool = startedPool(std::move(poolOptions));
        auto first = leaseFrom(run(pool->acquireTransfer()));
        const auto second = leaseFrom(run(pool->acquireTransfer()));
        auto command = leaseFrom(run(pool->acquireCommand()));
        ASSERT_FALSE(execute(command, Verb::Noop));
        giveBack(command);
        auto waiting = spawn(pool->acquireCommand());

        // The pool shows the refusal only through the limit it learned from it, 3 of the 4 its options allow.
        ASSERT_TRUE(eventually(
            [&pool]()
            {
                return pool->connectionLimit() == 3u;
            }
        ));
        flush(pool->strand());
        EXPECT_EQ(pool->health(), PoolHealth::Degraded);
        EXPECT_NE(waiting.wait_for(0s), std::future_status::ready) << "the refusal ended the wait for C0";

        giveBack(first);
        const auto again = leaseFrom(waitFor(waiting));
        EXPECT_EQ(again.name(), "C1");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_TRUE(losses_->recorded().empty());
        EXPECT_EQ(connectionsAccepted(), 4u);
    }

    TEST_F(PoolTests, C0ThatEndedBeforeThePoolKnewIsNotHandedOut)
    {
        server_->addConnection(idleTimeoutAfterLogin());
        Script again{};
        server_->addConnection(loggedIn(again).expect("NOOP").reply("200 NOOP ok.").expectDisconnect());
        const auto pool = startedPool();
        std::future<Pool::LeaseResult> command{};
        {
            // The command is queued before the end of C0 reaches the pool, only C0's own strand knows of it.
            const StrandHold hold{pool->strand()};
            command = spawn(pool->acquireCommand());
            ASSERT_NO_FATAL_FAILURE(endIdleMetadata());
        }

        const auto lease = leaseFrom(waitFor(command));
        const auto reply = execute(lease, Verb::Noop);
        ASSERT_TRUE(reply) << reply.error().toString();
        EXPECT_EQ(reply->code, 200);
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_EQ(connectionsAccepted(), 2u);
    }

    TEST_F(PoolTests, CommandBehindACancelledCheckOfAnEndedC0StillGetsC0)
    {
        asio::cancellation_signal signal{};
        server_->addConnection(idleTimeoutAfterLogin());
        server_->addConnection(loginThenDisconnect());
        const auto pool = startedPool();
        std::future<Pool::LeaseResult> cancelledCheck{};
        std::future<Pool::LeaseResult> waiting{};
        {
            // The first command checks C0, the second waits behind it, and the cancel reaches the first before the
            // check came back.
            const StrandHold hold{pool->strand()};
            cancelledCheck = spawn(pool->acquireCommand(), signal.slot());
            waiting = spawn(pool->acquireCommand());
            asio::post(
                pool->strand(),
                [&signal]()
                {
                    signal.emit(asio::cancellation_type::terminal);
                }
            );
            ASSERT_NO_FATAL_FAILURE(endIdleMetadata());
        }
        expectError(waitFor(cancelledCheck), RemoteErrorKind::Cancelled);

        EXPECT_EQ(leaseFrom(waitFor(waiting)).name(), "C0");
        EXPECT_EQ(pool->health(), PoolHealth::Healthy);
        EXPECT_EQ(connectionsAccepted(), 2u);
    }
}
