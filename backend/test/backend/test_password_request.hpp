#pragma once

#include <backend/password/password_request.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <string>

namespace Test
{
    TEST(PasswordRequestTests, WaitReturnsTheResolvedPassword)
    {
        PasswordRequest request;
        request.resolve(std::string{"secret"});
        EXPECT_EQ(request.wait(), std::optional<std::string>{"secret"});
    }

    TEST(PasswordRequestTests, OnlyTheFirstResolveCounts)
    {
        PasswordRequest request;
        request.resolve(std::nullopt);
        request.resolve(std::string{"late answer"});
        EXPECT_EQ(request.wait(), std::nullopt);
    }

    TEST(PasswordRequestTests, ResolveFromAnotherThreadWakesTheWaiter)
    {
        using namespace std::chrono_literals;

        PasswordRequest request;
        auto waiter = std::async(
            std::launch::async,
            [&request]()
            {
                return request.wait();
            }
        );
        EXPECT_EQ(waiter.wait_for(50ms), std::future_status::timeout);

        request.resolve(std::nullopt);
        ASSERT_EQ(waiter.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(waiter.get(), std::nullopt);
    }
}
