#pragma once

#include <backend/ftp/ftp_engine_context.hpp>

#include <boost/asio/post.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <stdexcept>

namespace Test
{
    namespace FtpEngineContextTesting
    {
        /**
         * @brief Whether all threads of an engine of @p threads threads still run handlers after as many handlers threw.
         */
        bool everyThreadGoesOnAfterAThrow(std::size_t threads)
        {
            std::atomic<std::size_t> running{0u};
            std::promise<void> allRunning{};
            std::promise<void> release{};
            const auto released = release.get_future().share();
            // Last, so it joins its threads before what their handlers use goes.
            FtpEngineContext engine{threads};
            for (std::size_t index = 0u; index < threads; ++index)
            {
                boost::asio::post(
                    engine.executor(),
                    []()
                    {
                        throw std::runtime_error{"handler failure"};
                    }
                );
            }

            // Every handler stays until all of them run, so each one needs a thread of its own that still runs.
            for (std::size_t index = 0u; index < threads; ++index)
            {
                boost::asio::post(
                    engine.executor(),
                    [&running, &allRunning, &engine, released, threads]()
                    {
                        if (engine.runningInThisThread() && ++running == threads)
                            allRunning.set_value();
                        released.wait();
                    }
                );
            }
            const bool all = allRunning.get_future().wait_for(std::chrono::seconds{10}) == std::future_status::ready;
            release.set_value();
            return all;
        }
    }

    TEST(FtpEngineContextTests, OneThreadGoesOnAfterAHandlerThrew)
    {
        FtpEngineContext engine{1u};
        EXPECT_EQ(engine.threads(), 1u);
        EXPECT_FALSE(engine.runningInThisThread());
        EXPECT_TRUE(FtpEngineContextTesting::everyThreadGoesOnAfterAThrow(1u));
    }

    TEST(FtpEngineContextTests, EveryOneOfMoreThreadsGoesOnAfterAHandlerThrew)
    {
        EXPECT_TRUE(FtpEngineContextTesting::everyThreadGoesOnAfterAThrow(3u));
    }

    TEST(FtpEngineContextTests, MoreThreadsRunTheHandlers)
    {
        FtpEngineContext engine{3u};
        EXPECT_EQ(engine.threads(), 3u);
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
        EXPECT_FALSE(engine.runningInThisThread());
    }
}
