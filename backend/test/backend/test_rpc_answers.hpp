#pragma once

#include <backend/rpc_helper.hpp>
#include "nui_env.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace Test
{
    TEST(RpcAnswerTests, AnAnswerOnCompletionAnswersWithTheResult)
    {
        if (!NuiEnv::instance().available())
            GTEST_SKIP() << "No display available";
        auto& environment = NuiEnv::instance();
        RpcHelper::RpcOnce const* answeredThrough = nullptr;
        std::optional<int> answeredWith{};

        auto complete = RpcHelper::answerOnCompletion(
            RpcHelper::RpcOnce{*environment.window, *environment.hub, "RpcAnswerTests::result"},
            [&](RpcHelper::RpcOnce const& reply, int result)
            {
                answeredThrough = &reply;
                answeredWith = result;
                reply({{"value", result}});
            }
        );
        complete(7);

        ASSERT_NE(answeredThrough, nullptr);
        EXPECT_EQ(answeredWith, 7);
        EXPECT_TRUE(answeredThrough->answered());
    }

    TEST(RpcAnswerTests, AnAnswerOnCompletionThatThrowsAnswersWithItsMessage)
    {
        if (!NuiEnv::instance().available())
            GTEST_SKIP() << "No display available";
        auto& environment = NuiEnv::instance();
        RpcHelper::RpcOnce const* answeredThrough = nullptr;

        auto complete = RpcHelper::answerOnCompletion(
            RpcHelper::RpcOnce{*environment.window, *environment.hub, "RpcAnswerTests::throws"},
            [&](RpcHelper::RpcOnce const& reply, int)
            {
                answeredThrough = &reply;
                throw std::runtime_error{"The answer broke"};
            }
        );
        EXPECT_NO_THROW(complete(7));

        ASSERT_NE(answeredThrough, nullptr);
        EXPECT_TRUE(answeredThrough->answered()) << "the exception was not answered";
    }
}
