#pragma once

#include <backend/ftp/ftp_engine_context.hpp>
#include <log/log.hpp>

#include <boost/asio/post.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <stdexcept>

namespace Test
{
    namespace FtpEngineContextTesting
    {
        /** @brief What the engine logs for the handler that throwOnTheEngine() lets throw. */
        constexpr auto loggedThrow = "FTP engine: A handler threw, the application ends: .*handler failure";

        /**
         * @brief Throws on an engine of @p threads threads with the errors logged to stderr, then waits for the end
         *        of the process that the throw has to bring.
         */
        [[noreturn]] void throwOnTheEngine(std::size_t threads)
        {
            Log::Logger::setupGlobalSinks(Log::Level::Error, {}, /*disableFileLog=*/true);
            Log::setLevel(Log::Level::Error);
            FtpEngineContext engine{threads};
            boost::asio::post(
                engine.executor(),
                []()
                {
                    throw std::runtime_error{"handler failure"};
                }
            );
            // Never set: only the end of the process ends the wait, a bounded one so a failure shows as a living
            // process instead of a hanging test.
            std::promise<void> never{};
            never.get_future().wait_for(std::chrono::seconds{10});
            std::_Exit(0);
        }
    }

    TEST(FtpEngineContextDeathTests, AHandlerThatThrowsOnTheOnlyThreadIsLoggedAndEndsTheProgram)
    {
        EXPECT_DEATH(FtpEngineContextTesting::throwOnTheEngine(1u), FtpEngineContextTesting::loggedThrow);
    }

    TEST(FtpEngineContextDeathTests, AHandlerThatThrowsOnOneOfMoreThreadsIsLoggedAndEndsTheProgram)
    {
        EXPECT_DEATH(FtpEngineContextTesting::throwOnTheEngine(3u), FtpEngineContextTesting::loggedThrow);
    }

    TEST(FtpEngineContextTests, OneThreadRunsTheHandlers)
    {
        FtpEngineContext engine{1u};
        EXPECT_EQ(engine.threads(), 1u);
        EXPECT_FALSE(engine.runningInThisThread());
        std::promise<bool> ran{};
        boost::asio::post(
            engine.executor(),
            [&ran, &engine]()
            {
                ran.set_value(engine.runningInThisThread());
            }
        );
        auto future = ran.get_future();
        ASSERT_EQ(future.wait_for(std::chrono::seconds{10}), std::future_status::ready);
        EXPECT_TRUE(future.get());
    }

    TEST(FtpEngineContextTests, EveryOneOfMoreThreadsRunsHandlers)
    {
        constexpr std::size_t threads = 3u;
        std::atomic<std::size_t> running{0u};
        std::promise<void> allRunning{};
        std::promise<void> release{};
        const auto released = release.get_future().share();
        // Last, so it joins its threads before what their handlers use goes.
        FtpEngineContext engine{threads};
        EXPECT_EQ(engine.threads(), threads);
        // Every handler stays until all of them run, so each one needs a thread of its own.
        for (std::size_t index = 0u; index < threads; ++index)
        {
            boost::asio::post(
                engine.executor(),
                [&running, &allRunning, &engine, released]()
                {
                    if (engine.runningInThisThread() && ++running == threads)
                        allRunning.set_value();
                    released.wait();
                }
            );
        }
        const bool all = allRunning.get_future().wait_for(std::chrono::seconds{10}) == std::future_status::ready;
        release.set_value();
        EXPECT_TRUE(all);
        EXPECT_FALSE(engine.runningInThisThread());
    }
}
