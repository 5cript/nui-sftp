#pragma once

#include "test_pool.hpp"

#include <ftp/engine/engine.hpp>
#include <ftp/engine/resolver.hpp>
#include <ftp/engine/task_gate.hpp>
#include <remote_io/async/processing_thread.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

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
        std::promise<void> release{};
        const auto released = release.get_future().share();
        std::promise<void> entered{};
        auto running = entered.get_future();
        ASSERT_TRUE(engine->post(
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
        EngineOptions options{};
        options.pool = PoolOptions{
            .host = server->address(),
            .port = server->port(),
            .login = {.credentials = {.user = "user", .password = "password"}},
            .control = {.connectTimeout = 10s, .commandTimeout = 10s},
        };
        const auto engine = makeEngine(std::move(options));
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

    INSTANTIATE_TEST_SUITE_P(
        Executors,
        EngineTests,
        ::testing::Values(EngineExecutor::OneThread, EngineExecutor::ThreadPool),
        [](::testing::TestParamInfo<EngineExecutor> const& info)
        {
            return info.param == EngineExecutor::OneThread ? std::string{"OneThread"} : std::string{"ThreadPool"};
        }
    );
}
