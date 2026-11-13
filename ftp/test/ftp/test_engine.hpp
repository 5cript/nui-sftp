#pragma once

#include "test_pool.hpp"

#include <ftp/engine/engine.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <remote_io/async/processing_thread.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace FileTransferProtocol::Test
{
    enum class EngineExecutor
    {
        OneThread,
        ThreadPool,
    };

    namespace
    {
        namespace asio = boost::asio;
        using namespace std::chrono_literals;

        /**
         * @brief The application's executor the engine is handed: one thread running an io_context, or a pool of
         *        two threads, where a missing strand shows.
         */
        class ExecutorHost
        {
          public:
            explicit ExecutorHost(EngineExecutor kind)
            {
                if (kind == EngineExecutor::ThreadPool)
                {
                    pool_.emplace(2u);
                    return;
                }
                context_.emplace(1);
                guard_.emplace(asio::make_work_guard(*context_));
                thread_ = std::thread{[context = &*context_]()
                    {
                        context->run();
                    }};
            }

            ~ExecutorHost()
            {
                if (!context_)
                    return;
                guard_.reset();
                context_->stop();
                thread_.join();
            }

            ExecutorHost(ExecutorHost const&) = delete;
            ExecutorHost& operator=(ExecutorHost const&) = delete;
            ExecutorHost(ExecutorHost&&) = delete;
            ExecutorHost& operator=(ExecutorHost&&) = delete;

            asio::any_io_executor executor()
            {
                if (pool_)
                    return pool_->get_executor();
                return context_->get_executor();
            }

          private:
            std::optional<asio::thread_pool> pool_{};
            std::optional<asio::io_context> context_{};
            std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_{};
            std::thread thread_{};
        };

        /**
         * @brief What the lost handler of an engine got, and on which thread.
         */
        struct LostReports
        {
            void record(RemoteIo::RemoteError const& error, bool onProcessingThread)
            {
                std::scoped_lock lock{mutex};
                errors.push_back(error);
                onTaskThread.push_back(onProcessingThread);
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
            std::vector<bool> onTaskThread{};
            std::promise<void> first{};
            std::future<void> reported{first.get_future()};
        };

        template <typename ValueT>
        ValueT valueOf(std::future<ValueT>& future)
        {
            if (future.wait_for(10s) != std::future_status::ready)
                throw std::runtime_error{"The wait did not end"};
            return future.get();
        }

        /**
         * @brief Waits until @p condition holds, at most 10 s.
         */
        bool waitUntil(std::function<bool()> const& condition)
        {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!condition() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            return condition();
        }
    }

    class EngineTests : public ::testing::TestWithParam<EngineExecutor>
    {
      protected:
        void SetUp() override
        {
            thread_.start();
        }

        void TearDown() override
        {
            if (engine_)
            {
                engine_->refusePosts();
                EXPECT_TRUE(engine_->waitIdle(5s)) << "work of the session was still in flight";
            }
            gate_->close();
            thread_.stop();
            engine_.reset();
        }

        std::shared_ptr<Engine> makeEngine(EngineOptions options = {})
        {
            engine_ = Engine::create(
                host_.executor(),
                std::make_shared<Resolver>(),
                gate_,
                std::move(options),
                [reports = reports_, gate = gate_](RemoteIo::RemoteError const& error)
                {
                    reports->record(error, gate->onProcessingThread());
                }
            );
            return engine_;
        }

        void expectOneLossThatSays(std::string const& text)
        {
            ASSERT_EQ(reports_->reported.wait_for(10s), std::future_status::ready) << "no loss was reported";
            const auto errors = reports_->recorded();
            ASSERT_EQ(errors.size(), 1u);
            EXPECT_EQ(errors.front().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << errors.front().toString();
            EXPECT_NE(errors.front().message.find(text), std::string::npos) << errors.front().message;
            EXPECT_TRUE(reports_->onTaskThread.front()) << "the lost handler ran outside the ProcessingThread";
        }

        static EngineOptions optionsFor(ScriptedFtpServer const& server)
        {
            EngineOptions options{};
            options.pool = PoolOptions{
                .host = server.address(),
                .port = server.port(),
                .login = {.credentials = {.user = "user", .password = "password"}},
                .control = {.connectTimeout = 10s, .commandTimeout = 10s},
            };
            return options;
        }

        static void expectNoFailures(ScriptedFtpServer& server)
        {
            ASSERT_TRUE(server.waitUntilDisconnected());
            for (auto const& failure : server.failures())
                ADD_FAILURE() << failure;
        }

        /**
         * @brief Engine::call from a task on the ProcessingThread.
         */
        template <typename ValueT, typename BodyT>
        std::future<Engine::Result<ValueT>>
        callFromTask(std::shared_ptr<Engine> const& engine, BodyT body, SyncLimits limits)
        {
            return thread_.pushPromiseTask(
                [engine, body = std::move(body), limits]() mutable
                {
                    return engine->call<ValueT>(std::move(body), limits);
                }
            );
        }

        /**
         * @brief Whether the pool is gone, asked on the session strand.
         */
        bool poolGone(std::shared_ptr<Engine> const& engine)
        {
            std::promise<bool> gone{};
            auto answer = gone.get_future();
            asio::post(
                engine->strand(),
                [&gone, engine]()
                {
                    gone.set_value(engine->pool() == nullptr);
                }
            );
            return valueOf(answer);
        }

      protected:
        ExecutorHost host_{GetParam()};
        RemoteIo::ProcessingThread thread_{};
        std::shared_ptr<TaskGate> gate_ = std::make_shared<TaskGate>(thread_);
        std::shared_ptr<LostReports> reports_ = std::make_shared<LostReports>();
        std::shared_ptr<Engine> engine_{};
    };

    TEST_P(EngineTests, PostRunsOnTheSessionStrand)
    {
        const auto engine = makeEngine();
        std::promise<bool> ran{};
        auto onStrand = ran.get_future();
        ASSERT_TRUE(engine->post(
            [&ran, engine]()
            {
                ran.set_value(engine->strand().running_in_this_thread() && engine->pool() != nullptr);
            }
        ));
        EXPECT_TRUE(valueOf(onStrand));
    }

    TEST_P(EngineTests, RefusedPostsAndSpawnsRunNothing)
    {
        const auto engine = makeEngine();
        engine->refusePosts();
        auto ran = false;
        EXPECT_FALSE(engine->post(
            [&ran]()
            {
                ran = true;
            }
        ));
        EXPECT_FALSE(engine->spawn(
            [](bool* flag) -> asio::awaitable<void>
            {
                *flag = true;
                co_return;
            }(&ran)
        ));
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(ran);
    }

    TEST_P(EngineTests, WaitIdleWaitsForWorkInFlight)
    {
        const auto engine = makeEngine();
        std::promise<void> release{};
        const auto released = release.get_future().share();
        std::promise<void> entered{};
        auto running = entered.get_future();
        ASSERT_TRUE(engine->post(
            [&entered, released]()
            {
                entered.set_value();
                released.wait();
            }
        ));
        valueOf(running);
        // Only the clock tells a wait that has not ended yet.
        EXPECT_FALSE(engine->waitIdle(20ms));
        release.set_value();
        EXPECT_TRUE(engine->waitIdle(5s));
    }

    TEST_P(EngineTests, ExceptionInAPostFailsTheSessionOnce)
    {
        const auto engine = makeEngine();
        for (auto const* const what : {"boom", "again"})
        {
            ASSERT_TRUE(engine->post(
                [what]()
                {
                    throw std::runtime_error{what};
                }
            ));
        }
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("boom"));
        EXPECT_TRUE(engine->failed());
        EXPECT_TRUE(poolGone(engine));
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_EQ(reports_->recorded().size(), 1u);
    }

    TEST_P(EngineTests, ExceptionInASpawnFailsTheSession)
    {
        const auto engine = makeEngine();
        ASSERT_TRUE(engine->spawn(
            []() -> asio::awaitable<void>
            {
                throw std::runtime_error{"the coroutine broke"};
                co_return;
            }()
        ));
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("the coroutine broke"));
        EXPECT_TRUE(engine->failed());
        EXPECT_TRUE(poolGone(engine));
    }

    TEST_P(EngineTests, ExceptionOnAConnectionStrandFailsTheSession)
    {
        const auto engine = makeEngine();
        const auto connectionStrand = asio::make_strand(host_.executor());
        ASSERT_TRUE(engine->postTo(
            connectionStrand,
            []()
            {
                throw std::runtime_error{"a connection handler broke"};
            }
        ));
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("a connection handler broke"));
        EXPECT_TRUE(poolGone(engine));
    }

    TEST_P(EngineTests, ExceptionAfterRefusePostsStillFailsTheSession)
    {
        const auto engine = makeEngine();
        const auto connectionStrand = asio::make_strand(host_.executor());
        std::promise<void> release{};
        const auto released = release.get_future().share();
        std::promise<void> entered{};
        auto running = entered.get_future();
        ASSERT_TRUE(engine->postTo(
            connectionStrand,
            [&entered, released]()
            {
                entered.set_value();
                released.wait();
                throw std::runtime_error{"a handler broke during the teardown"};
            }
        ));
        valueOf(running);
        engine->refusePosts();
        release.set_value();
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("a handler broke during the teardown"));
        EXPECT_TRUE(engine->failed());
        EXPECT_TRUE(poolGone(engine));
    }

    TEST_P(EngineTests, EmptyTasksAreRefused)
    {
        const auto engine = makeEngine();
        EXPECT_FALSE(engine->pushTask(std::function<void()>{}));
    }

    TEST_P(EngineTests, TasksGoToTheProcessingThreadUntilTheGateCloses)
    {
        const auto engine = makeEngine();
        EXPECT_FALSE(engine->onProcessingThread());
        std::promise<bool> ran{};
        auto onTaskThread = ran.get_future();
        ASSERT_TRUE(engine->pushTask(
            [&ran, engine]()
            {
                ran.set_value(engine->onProcessingThread());
            }
        ));
        EXPECT_TRUE(valueOf(onTaskThread));

        gate_->close();
        EXPECT_FALSE(engine->pushTask([]() {}));
    }

    TEST_P(EngineTests, StoppingIsSeenFromAnyThread)
    {
        const auto engine = makeEngine();
        EXPECT_FALSE(engine->stopping());
        std::thread{
            [engine]()
            {
                engine->setStopping();
            }
        }.join();
        EXPECT_TRUE(engine->stopping());
        EXPECT_FALSE(engine->failed());
    }

    TEST_P(EngineTests, PoolLossAndALaterFailureReportOnce)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        server->addConnection(loginThenResetOn("NOOP"));
        Script refused{};
        server->addConnection(refused.reply("220 Ready")
                .expect("USER", "user")
                .reply("331 Password required")
                .expect("PASS", "password")
                .reply("530 Login incorrect.")
                .expectDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        const auto pool = engine->pool();
        auto start = asio::co_spawn(engine->strand(), pool->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));
        auto acquired = asio::co_spawn(engine->strand(), pool->acquireCommand(), asio::use_future);
        auto lease = valueOf(acquired);
        ASSERT_TRUE(lease) << lease.error().toString();
        const auto control = lease->control();
        auto noop = asio::co_spawn(control->strand(), control->execute(Command{.verb = Verb::Noop}), asio::use_future);
        ASSERT_FALSE(valueOf(noop)) << "the reset connection answered";
        lease->release();

        ASSERT_EQ(reports_->reported.wait_for(10s), std::future_status::ready) << "the pool's loss was not reported";
        ASSERT_TRUE(engine->post(
            []()
            {
                throw std::runtime_error{"a failure after the loss"};
            }
        ));
        EXPECT_TRUE(poolGone(engine));
        EXPECT_TRUE(engine->failed());
        // A report of the failure would be queued before this task.
        auto behind = thread_.pushPromiseTask([]() {});
        behind.wait();
        const auto errors = reports_->recorded();
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front().kind, RemoteIo::RemoteErrorKind::AuthenticationFailed) << errors.front().toString();
        ASSERT_TRUE(server->waitUntilDisconnected());
        EXPECT_TRUE(server->failures().empty());
    }

    TEST_P(EngineTests, CallReturnsWhatItsBodyReturnedOnTheSessionStrand)
    {
        const auto engine = makeEngine();
        auto result = callFromTask<bool>(
            engine,
            [engine](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<bool>>
            {
                co_return engine->strand().running_in_this_thread();
            },
            SyncLimits{}
        );
        const auto onStrand = valueOf(result);
        ASSERT_TRUE(onStrand) << onStrand.error().toString();
        EXPECT_TRUE(*onStrand);
    }

    TEST_P(EngineTests, CallIsRefusedOutsideTheProcessingThread)
    {
        const auto engine = makeEngine();
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        const auto body = [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
        {
            *ran = true;
            co_return 1;
        };
        const auto fromTestThread = engine->call<int>(body, SyncLimits{});
        ASSERT_FALSE(fromTestThread);
        EXPECT_EQ(fromTestThread.error().kind, RemoteIo::RemoteErrorKind::Failure);

        std::promise<Engine::Result<int>> fromStrand{};
        auto answer = fromStrand.get_future();
        ASSERT_TRUE(engine->post(
            [&fromStrand, engine, body]()
            {
                fromStrand.set_value(engine->call<int>(body, SyncLimits{}));
            }
        ));
        const auto onStrand = valueOf(answer);
        ASSERT_FALSE(onStrand);
        EXPECT_NE(onStrand.error().message.find("session strand"), std::string::npos) << onStrand.error().message;
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(*ran);
    }

    TEST_P(EngineTests, CallIsRefusedWhileTheSessionStopsAndOnceItFailed)
    {
        const auto engine = makeEngine();
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        const auto body = [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
        {
            *ran = true;
            co_return 1;
        };
        engine->setStopping();
        auto whileStopping = callFromTask<int>(engine, body, SyncLimits{});
        const auto stopping = valueOf(whileStopping);
        ASSERT_FALSE(stopping);
        EXPECT_EQ(stopping.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << stopping.error().toString();

        ASSERT_TRUE(engine->post(
            []()
            {
                throw std::runtime_error{"boom"};
            }
        ));
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("boom"));
        auto afterFailure = callFromTask<int>(engine, body, SyncLimits{});
        const auto failed = valueOf(afterFailure);
        ASSERT_FALSE(failed);
        EXPECT_NE(failed.error().message.find("failed"), std::string::npos) << failed.error().message;
        EXPECT_FALSE(*ran);
    }

    TEST_P(EngineTests, TheEndOfItsGroupEndsAWaitingCallAtOnceAndCancelsItsOperation)
    {
        const auto engine = makeEngine();
        const auto group = std::make_shared<CallGroup>();
        const auto started = std::make_shared<std::promise<void>>();
        const auto ended = std::make_shared<std::promise<bool>>();
        auto operationEnded = ended->get_future();
        const auto body = [started, ended](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
        {
            started->set_value();
            asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
            const auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
            ended->set_value(error == asio::error::operation_aborted);
            co_return std::unexpected(
                RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::Cancelled, .message = "The wait was cancelled"}
            );
        };
        auto result = thread_.pushPromiseTask(
            [engine, body, group]()
            {
                return engine->call<int>(body, SyncLimits{.backstop = 1h, .drain = 5s}, group);
            }
        );
        ASSERT_EQ(started->get_future().wait_for(10s), std::future_status::ready);

        group->end(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::ConnectionLost, .message = "closed"});
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().message, "closed") << outcome.error().toString();
        ASSERT_EQ(operationEnded.wait_for(10s), std::future_status::ready) << "the operation ran on";
        EXPECT_TRUE(operationEnded.get()) << "the operation was not cancelled";
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(engine->failed());
    }

    TEST_P(EngineTests, ACallOfAnEndedGroupIsRefusedBeforeItsOperationStarts)
    {
        const auto engine = makeEngine();
        const auto group = std::make_shared<CallGroup>();
        group->end(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::ConnectionLost, .message = "closed"});
        group->end(RemoteIo::RemoteError{.kind = RemoteIo::RemoteErrorKind::Failure, .message = "later"});
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        auto result = thread_.pushPromiseTask(
            [engine, group, ran]()
            {
                return engine->call<int>(
                    [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                    {
                        *ran = true;
                        co_return 1;
                    },
                    SyncLimits{},
                    group
                );
            }
        );
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().message, "closed") << outcome.error().toString();
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(*ran);
    }

    TEST_P(EngineTests, RunCompletesOnTheCallingThreadWhenPostsAreRefused)
    {
        const auto engine = makeEngine();
        engine->refusePosts();
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        std::optional<Engine::Result<int>> answer{};
        engine->run<int>(
            [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                *ran = true;
                co_return 1;
            },
            SyncLimits{},
            [&answer](Engine::Result<int> result)
            {
                answer = std::move(result);
            }
        );
        ASSERT_TRUE(answer) << "the refused operation did not complete";
        ASSERT_FALSE(*answer);
        EXPECT_EQ(answer->error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << answer->error().toString();
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(*ran);
    }

    TEST_P(EngineTests, ABodyCancelledAtTheBackstopMayStillAwaitAndEndsAsTimedOut)
    {
        const auto engine = makeEngine();
        const auto cancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cleanedUp = std::make_shared<std::atomic<bool>>(false);
        const auto limits = SyncLimits{.backstop = 100ms, .drain = 5s};
        const auto begin = std::chrono::steady_clock::now();
        auto result = callFromTask<int>(
            engine,
            [cancelled, cleanedUp](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                const auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                *cancelled = error == asio::error::operation_aborted;
                co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
                *cleanedUp = true;
                co_return std::unexpected(
                    RemoteIo::RemoteError{
                        .kind = RemoteIo::RemoteErrorKind::Cancelled,
                        .message = "The wait was cancelled",
                    }
                );
            },
            limits
        );
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << outcome.error().toString();
        EXPECT_GE(std::chrono::steady_clock::now() - begin, limits.backstop);
        EXPECT_TRUE(*cancelled);
        EXPECT_TRUE(*cleanedUp);
        EXPECT_FALSE(engine->failed());
    }

    TEST_P(EngineTests, AnOperationThatAllowsItselfMoreRunsPastItsBackstop)
    {
        const auto engine = makeEngine();
        const auto limits = SyncLimits{.backstop = 100ms, .drain = 5s};
        const auto begin = std::chrono::steady_clock::now();
        auto result = callFromTask<int>(
            engine,
            [](std::shared_ptr<CallScope> scope) -> asio::awaitable<Engine::Result<int>>
            {
                scope->allowMore(1h);
                asio::steady_timer timer{co_await asio::this_coro::executor, 300ms};
                const auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                if (error)
                {
                    co_return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteIo::RemoteErrorKind::Cancelled,
                            .message = "The wait was cancelled",
                        }
                    );
                }
                co_return 7;
            },
            limits
        );
        const auto outcome = valueOf(result);
        ASSERT_TRUE(outcome) << outcome.error().toString();
        EXPECT_EQ(*outcome, 7);
        EXPECT_GE(std::chrono::steady_clock::now() - begin, 300ms);
    }

    TEST_P(EngineTests, AnOperationThatAllowedItselfMoreEndsAtTheLaterBackstop)
    {
        const auto engine = makeEngine();
        const auto limits = SyncLimits{.backstop = 100ms, .drain = 5s};
        const auto begin = std::chrono::steady_clock::now();
        auto result = callFromTask<int>(
            engine,
            [](std::shared_ptr<CallScope> scope) -> asio::awaitable<Engine::Result<int>>
            {
                scope->allowMore(200ms);
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return std::unexpected(
                    RemoteIo::RemoteError{
                        .kind = RemoteIo::RemoteErrorKind::Cancelled,
                        .message = "The wait was cancelled",
                    }
                );
            },
            limits
        );
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome) << "an operation ran past the backstop it had moved";
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << outcome.error().toString();
        EXPECT_GE(std::chrono::steady_clock::now() - begin, 300ms);
    }

    TEST_P(EngineTests, AnUncancellableBodyIsGivenUpAfterTheDrainAndItsConnectionsClose)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        server->addConnection(loginThenDisconnect());
        server->addConnection(loginThenDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        const auto pool = engine->pool();
        auto start = asio::co_spawn(engine->strand(), pool->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));

        const auto blocker =
            std::make_shared<asio::steady_timer>(engine->strand(), asio::steady_timer::time_point::max());
        const auto touched = std::make_shared<std::promise<std::shared_ptr<ControlConnection>>>();
        auto used = touched->get_future();
        const auto limits = SyncLimits{.backstop = 200ms, .drain = 100ms};
        const auto begin = std::chrono::steady_clock::now();
        auto result = callFromTask<int>(
            engine,
            [pool, blocker, touched](std::shared_ptr<CallScope> scope) -> asio::awaitable<Engine::Result<int>>
            {
                auto lease = co_await pool->acquireTransfer();
                if (!lease)
                    co_return std::unexpected(lease.error());
                scope->touch(lease->control());
                touched->set_value(lease->control());
                // The cancel does not reach this wait, as with an operation that ignores it.
                co_await blocker->async_wait(
                    asio::bind_cancellation_slot(asio::cancellation_slot{}, asio::as_tuple(asio::use_awaitable))
                );
                co_return 7;
            },
            limits
        );
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome) << "an operation that could not end answered";
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << outcome.error().toString();
        EXPECT_GE(std::chrono::steady_clock::now() - begin, limits.backstop + limits.drain);

        const auto control = valueOf(used);
        std::promise<ControlState> state{};
        auto closed = state.get_future();
        asio::post(
            control->strand(),
            [&state, control]()
            {
                state.set_value(control->state());
            }
        );
        EXPECT_EQ(valueOf(closed), ControlState::Closed);

        // Only the clock tells a wait that has not ended yet.
        EXPECT_FALSE(engine->waitIdle(20ms)) << "the body that still runs is not counted";
        asio::post(
            engine->strand(),
            [blocker]()
            {
                blocker->cancel();
            }
        );
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(engine->failed()) << "the late result was not dropped";
    }

    TEST_P(EngineTests, FailingTheSessionEndsTheOperationsInFlight)
    {
        const auto engine = makeEngine();
        const auto entered = std::make_shared<std::promise<void>>();
        auto waiting = entered->get_future();
        auto result = callFromTask<int>(
            engine,
            [entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                entered->set_value();
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return 1;
            },
            SyncLimits{.backstop = 1h, .drain = 1h}
        );
        valueOf(waiting);
        ASSERT_TRUE(engine->post(
            []()
            {
                throw std::runtime_error{"broke during an operation"};
            }
        ));
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_NE(outcome.error().message.find("broke during an operation"), std::string::npos)
            << outcome.error().message;
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("broke during an operation"));
        EXPECT_TRUE(engine->waitIdle(5s)) << "the cancel did not reach the operation";
    }

    TEST_P(EngineTests, ABodyThatThrowsFailsTheSessionAndItsCall)
    {
        const auto engine = makeEngine();
        auto result = callFromTask<int>(
            engine,
            [](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
                throw std::runtime_error{"the operation broke"};
            },
            SyncLimits{}
        );
        const auto outcome = valueOf(result);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_NE(outcome.error().message.find("the operation broke"), std::string::npos) << outcome.error().message;
        ASSERT_NO_FATAL_FAILURE(expectOneLossThatSays("the operation broke"));
        EXPECT_TRUE(poolGone(engine));
    }

    TEST_P(EngineTests, CloseSaysQuitAndEndsTheSession)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        Script metadata{};
        server->addConnection(loggedIn(metadata).expect("QUIT").reply("221 Goodbye.").expectDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        auto start = asio::co_spawn(engine->strand(), engine->pool()->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));

        EXPECT_TRUE(engine->close());
        EXPECT_TRUE(engine->stopping());
        EXPECT_FALSE(engine->post([]() {}));
        EXPECT_FALSE(engine->pushTask([]() {}));
        EXPECT_FALSE(thread_.pushTask([]() {})) << "the ProcessingThread still runs";
        EXPECT_FALSE(engine->failed());
        ASSERT_NO_FATAL_FAILURE(expectNoFailures(*server));
        EXPECT_TRUE(reports_->recorded().empty()) << "the close was reported as a loss";
    }

    TEST_P(EngineTests, AStuckCallDoesNotKeepTheCloseFromSayingQuit)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        Script metadata{};
        server->addConnection(loggedIn(metadata).expect("QUIT").reply("221 Goodbye.").expectDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        auto start = asio::co_spawn(engine->strand(), engine->pool()->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));
        const auto entered = std::make_shared<std::promise<void>>();
        auto waiting = entered->get_future();
        auto stuck = callFromTask<int>(
            engine,
            [entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                entered->set_value();
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return 1;
            },
            SyncLimits{.backstop = 1h, .drain = 1h}
        );
        valueOf(waiting);

        EXPECT_TRUE(engine->close());
        const auto outcome = valueOf(stuck);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        ASSERT_NO_FATAL_FAILURE(expectNoFailures(*server));
    }

    TEST_P(EngineTests, ACallStartedWhileTheCloseRunsIsRefusedSoQuitStillGoesOut)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        Script metadata{};
        server->addConnection(loggedIn(metadata).expect("QUIT").reply("221 Goodbye.").expectDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        auto start = asio::co_spawn(engine->strand(), engine->pool()->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));
        const auto entered = std::make_shared<std::promise<void>>();
        auto waiting = entered->get_future();
        const auto followUpRan = std::make_shared<std::atomic<bool>>(false);
        auto followUp = thread_.pushPromiseTask(
            [engine, entered, followUpRan]()
            {
                const auto stuck = [entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                {
                    asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                    entered->set_value();
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    co_return 1;
                };
                const auto first = engine->call<int>(stuck, SyncLimits{.backstop = 1h, .drain = 1h});
                EXPECT_FALSE(first) << "the close did not fail the call running then";
                // A task that goes on after its call failed, as a sequence of commands does.
                return engine->call<int>(
                    [followUpRan](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                    {
                        *followUpRan = true;
                        asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                        co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                        co_return 2;
                    },
                    SyncLimits{.backstop = 1h, .drain = 1h}
                );
            }
        );
        valueOf(waiting);

        EXPECT_TRUE(engine->close());
        const auto outcome = valueOf(followUp);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_FALSE(*followUpRan) << "an operation started while the close waited for QUIT";
        ASSERT_NO_FATAL_FAILURE(expectNoFailures(*server));
    }

    TEST_P(EngineTests, ACloseWhoseQuitCannotRunEndsTheConnectionsAndTheLeftoverTasks)
    {
        auto started = ScriptedFtpServer::start();
        ASSERT_TRUE(started) << started.error();
        const auto server = std::move(started).value();
        server->addConnection(loginThenDisconnect());
        const auto engine = makeEngine(optionsFor(*server));
        auto start = asio::co_spawn(engine->strand(), engine->pool()->start(), asio::use_future);
        ASSERT_TRUE(valueOf(start));
        ASSERT_TRUE(thread_.pushTask(
            [engine]()
            {
                // Holds the thread until the close stops the session, the QUIT and the tasks behind it are left over.
                EXPECT_TRUE(waitUntil(
                    [&engine]()
                    {
                        return engine->stopping();
                    }
                ));
            }
        ));
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        auto leftover = callFromTask<int>(
            engine,
            [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                *ran = true;
                co_return 1;
            },
            SyncLimits{}
        );

        EXPECT_TRUE(engine->close(TeardownOptions{.quitBackstop = 50ms, .drain = 50ms}));
        ASSERT_EQ(leftover.wait_for(0s), std::future_status::ready) << "the close returned before the leftover tasks";
        const auto outcome = leftover.get();
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_FALSE(*ran);
        ASSERT_NO_FATAL_FAILURE(expectNoFailures(*server));
        EXPECT_TRUE(reports_->recorded().empty()) << "the close was reported as a loss";
    }

    TEST_P(EngineTests, ACallOfACleanupTaskAfterTheCloseIsRefusedAsClosed)
    {
        const auto engine = makeEngine();
        EXPECT_TRUE(engine->close());
        const auto ran = std::make_shared<std::atomic<bool>>(false);
        std::optional<Engine::Result<int>> answer{};
        thread_.pushCleanupTask(
            [&answer, engine, ran]()
            {
                answer = engine->call<int>(
                    [ran](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                    {
                        *ran = true;
                        co_return 1;
                    },
                    SyncLimits{}
                );
            }
        );
        ASSERT_TRUE(answer) << "the cleanup task did not run on the closing thread";
        ASSERT_FALSE(*answer);
        EXPECT_EQ(answer->error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << answer->error().toString();
        EXPECT_FALSE(*ran);
    }

    TEST_P(EngineTests, ACloseAfterPostsWereRefusedStillEndsTheSession)
    {
        const auto engine = makeEngine();
        const auto entered = std::make_shared<std::promise<void>>();
        auto waiting = entered->get_future();
        const auto answer = std::make_shared<std::promise<Engine::Result<int>>>();
        auto answered = answer->get_future();
        engine->run<int>(
            [entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                entered->set_value();
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return 1;
            },
            SyncLimits{.backstop = 1h, .drain = 1h},
            [answer](Engine::Result<int> result)
            {
                answer->set_value(std::move(result));
            }
        );
        valueOf(waiting);
        engine->refusePosts();

        EXPECT_TRUE(engine->close()) << "the operation in flight was not ended";
        const auto outcome = valueOf(answered);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        EXPECT_TRUE(poolGone(engine)) << "the pool outlived the close";
    }

    TEST_P(EngineTests, ASecondCloseReturnsOnlyAfterTheFirst)
    {
        const auto engine = makeEngine();
        ASSERT_TRUE(thread_.pushTask(
            [engine]()
            {
                // Holds the thread until the first close stops the session, so that close waits for its QUIT.
                EXPECT_TRUE(waitUntil(
                    [&engine]()
                    {
                        return engine->stopping();
                    }
                ));
            }
        ));
        const auto failedByTheFirst = std::make_shared<std::promise<void>>();
        auto firstRuns = failedByTheFirst->get_future();
        engine->run<int>(
            [](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                co_return 1;
            },
            SyncLimits{.backstop = 1h, .drain = 1h},
            [failedByTheFirst](Engine::Result<int>)
            {
                failedByTheFirst->set_value();
            }
        );
        const auto options = TeardownOptions{.quitBackstop = 50ms, .drain = 50ms};
        auto first = std::async(
            std::launch::async,
            [engine, options]()
            {
                return engine->close(options);
            }
        );
        valueOf(firstRuns);

        EXPECT_TRUE(engine->close(options));
        EXPECT_FALSE(engine->post([]() {})) << "the second close returned while the first still ran";
        EXPECT_TRUE(valueOf(first));
    }

    TEST_P(EngineTests, AnOperationThatOutlivesTheCloseOwnsItsState)
    {
        const auto engine = makeEngine();
        const auto blocker =
            std::make_shared<asio::steady_timer>(engine->strand(), asio::steady_timer::time_point::max());
        const auto entered = std::make_shared<std::promise<void>>();
        auto waiting = entered->get_future();
        const auto answer = std::make_shared<std::promise<Engine::Result<int>>>();
        auto answered = answer->get_future();
        engine->run<int>(
            [blocker, entered](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
            {
                entered->set_value();
                co_await blocker->async_wait(
                    asio::bind_cancellation_slot(asio::cancellation_slot{}, asio::as_tuple(asio::use_awaitable))
                );
                co_return 7;
            },
            SyncLimits{.backstop = 1h, .drain = 1h},
            [answer](Engine::Result<int> result)
            {
                answer->set_value(std::move(result));
            }
        );
        valueOf(waiting);

        EXPECT_FALSE(engine->close(TeardownOptions{.quitBackstop = 50ms, .drain = 50ms, .closeGrace = 50ms}))
            << "the operation that still runs is not counted";
        const auto outcome = valueOf(answered);
        ASSERT_FALSE(outcome);
        EXPECT_EQ(outcome.error().kind, RemoteIo::RemoteErrorKind::ConnectionLost) << outcome.error().toString();
        asio::post(
            engine->strand(),
            [blocker]()
            {
                blocker->cancel();
            }
        );
        EXPECT_TRUE(engine->waitIdle(5s));
        EXPECT_FALSE(engine->failed()) << "the late result was not dropped";
    }

    TEST_P(EngineTests, TheEngineRunsOnWhileTheProcessingThreadIsHeld)
    {
        /**
         * @brief Lets the holding task go at the end of a round, also when an assertion ends it early.
         */
        struct Hold
        {
            Hold() = default;
            ~Hold()
            {
                *release = true;
                unblock.set_value();
            }
            Hold(Hold const&) = delete;
            Hold& operator=(Hold const&) = delete;
            Hold(Hold&&) = delete;
            Hold& operator=(Hold&&) = delete;

            const std::shared_ptr<std::atomic<bool>> release = std::make_shared<std::atomic<bool>>(false);
            std::promise<void> unblock{};
        };

        const auto engine = makeEngine();
        for (const auto busy : {false, true})
        {
            SCOPED_TRACE(busy ? "a busy task" : "a blocked task");
            Hold hold{};
            const auto release = hold.release;
            const auto unblocked = hold.unblock.get_future().share();
            ASSERT_TRUE(thread_.pushTask(
                [release, unblocked, busy]()
                {
                    if (!busy)
                    {
                        unblocked.wait();
                        return;
                    }
                    while (!release->load())
                    {}
                }
            ));

            const auto timed = std::make_shared<std::promise<Engine::Result<int>>>();
            auto timedAnswer = timed->get_future();
            engine->run<int>(
                [](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                {
                    asio::steady_timer timer{co_await asio::this_coro::executor, 20ms};
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    co_return 1;
                },
                SyncLimits{},
                [timed](Engine::Result<int> result)
                {
                    timed->set_value(std::move(result));
                }
            );
            const auto cut = std::make_shared<std::promise<Engine::Result<int>>>();
            auto cutAnswer = cut->get_future();
            engine->run<int>(
                [](std::shared_ptr<CallScope>) -> asio::awaitable<Engine::Result<int>>
                {
                    asio::steady_timer timer{co_await asio::this_coro::executor, 1h};
                    co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    co_return std::unexpected(
                        RemoteIo::RemoteError{
                            .kind = RemoteIo::RemoteErrorKind::Cancelled,
                            .message = "The wait was cancelled",
                        }
                    );
                },
                SyncLimits{.backstop = 50ms, .drain = 5s},
                [cut](Engine::Result<int> result)
                {
                    cut->set_value(std::move(result));
                }
            );

            // Both answer while the task still holds the ProcessingThread.
            const auto timedOutcome = valueOf(timedAnswer);
            ASSERT_TRUE(timedOutcome) << timedOutcome.error().toString();
            EXPECT_EQ(*timedOutcome, 1);
            const auto cutOutcome = valueOf(cutAnswer);
            ASSERT_FALSE(cutOutcome);
            EXPECT_EQ(cutOutcome.error().kind, RemoteIo::RemoteErrorKind::TimedOut) << cutOutcome.error().toString();
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        EngineTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );

    TEST(EngineCallTests, ACallCompletesOnceAndAsLostWhenDropped)
    {
        asio::io_context context{};
        std::vector<Engine::Result<int>> results{};
        const auto record = [&results](Engine::Result<int> result)
        {
            results.push_back(std::move(result));
        };
        {
            Detail::Call<int> call{context.get_executor(), SyncLimits{}, record};
            call.complete(1);
            call.complete(2);
            EXPECT_TRUE(call.completed());
        }
        ASSERT_EQ(results.size(), 1u);
        EXPECT_EQ(results.front(), 1);

        results.clear();
        {
            Detail::Call<int> dropped{context.get_executor(), SyncLimits{}, record};
        }
        ASSERT_EQ(results.size(), 1u);
        ASSERT_FALSE(results.front());
        EXPECT_EQ(results.front().error().kind, RemoteIo::RemoteErrorKind::ConnectionLost);
    }
}
