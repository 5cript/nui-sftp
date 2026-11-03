#pragma once

#include <backend/rpc_helper.hpp>
#include "nui_env.hpp"
#include "recording_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>

namespace Test
{
    TEST(RpcAnswerTests, AnAnswerOnCompletionAnswersWithTheResult)
    {
        const RecordingReply reply{};

        auto complete = RpcHelper::answerOnCompletion(
            reply,
            [](RecordingReply const& reply, int result)
            {
                reply({{"value", result}});
            }
        );
        complete(7);

        ASSERT_EQ(reply.answered().wait_for(std::chrono::seconds{0}), std::future_status::ready);
        const auto answer = reply.answered().get();
        ASSERT_TRUE(answer.json.has_value()) << "answered with an error: " << answer.error.value_or("");
        EXPECT_EQ(*answer.json, (nlohmann::json{{"value", 7}}));
        EXPECT_EQ(reply.answers(), 1u);
    }

    TEST(RpcAnswerTests, AnAnswerOnCompletionThatThrowsAnswersWithItsMessage)
    {
        const RecordingReply reply{};

        auto complete = RpcHelper::answerOnCompletion(
            reply,
            [](RecordingReply const&, int)
            {
                throw std::runtime_error{"The answer broke"};
            }
        );
        EXPECT_NO_THROW(complete(7));

        ASSERT_EQ(reply.answered().wait_for(std::chrono::seconds{0}), std::future_status::ready);
        const auto answer = reply.answered().get();
        EXPECT_FALSE(answer.json.has_value());
        EXPECT_EQ(answer.error, "The answer broke");
        EXPECT_EQ(reply.answers(), 1u);
    }

    TEST(RpcAnswerTests, AnAnswerOnCompletionTakesTheRpcOnceAlong)
    {
        if (!NuiEnv::instance().available())
            GTEST_SKIP() << "No display available";
        auto& environment = NuiEnv::instance();
        RpcHelper::RpcOnce reply{*environment.window, *environment.hub, "RpcAnswerTests::result"};
        RpcHelper::RpcOnce const* answeredThrough = nullptr;

        auto complete = RpcHelper::answerOnCompletion(
            std::move(reply),
            [&answeredThrough](RpcHelper::RpcOnce const& reply, int result)
            {
                answeredThrough = &reply;
                reply({{"value", result}});
            }
        );
        complete(7);

        ASSERT_NE(answeredThrough, nullptr);
        EXPECT_TRUE(answeredThrough->answered());
    }
}
