#pragma once

#include <backend/ftp/ftp_engine_context.hpp>

#include <boost/asio/post.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>

namespace Test
{
    TEST(FtpEngineContextTests, OneThreadGoesOnAfterAHandlerThrew)
    {
        FtpEngineContext engine{1u};
        EXPECT_EQ(engine.threads(), 1u);
        EXPECT_FALSE(engine.runningInThisThread());
        boost::asio::post(
            engine.executor(),
            []()
            {
                throw std::runtime_error{"handler failure"};
            }
        );
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

    TEST(FtpEngineContextTests, MoreThreadsRunAsAPool)
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
