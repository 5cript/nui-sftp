#pragma once

#include <utility/lifetime_token.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace Utility::Test
{
    TEST(LifetimeTokenTests, AGuardedCallbackRunsOnlyWhileItsTokenExists)
    {
        int calls = 0;
        std::function<void(int)> callback;
        {
            const LifetimeToken token;
            callback = token.guarded(
                [&calls](int amount)
                {
                    calls += amount;
                }
            );
            callback(2);
            EXPECT_EQ(calls, 2);
            using Guarded = decltype(token.guarded([](int) {}));
            static_assert(std::is_invocable_v<Guarded, int>);
            static_assert(!std::is_invocable_v<Guarded, std::string>, "the wrapper keeps the parameter types");
        }
        callback(5);
        EXPECT_EQ(calls, 2) << "the owner is gone";
    }

    TEST(LifetimeTokenTests, ACopiedOrMovedOwnerHasATokenOfItsOwn)
    {
        int calls = 0;
        auto original = std::make_optional<LifetimeToken>();
        auto callback = original->guarded(
            [&calls]()
            {
                ++calls;
            }
        );
        const LifetimeToken copy{*original};
        const LifetimeToken moved{std::move(*original)};
        LifetimeToken assigned;
        assigned = copy;
        callback();
        EXPECT_EQ(calls, 1) << "the original still exists after being moved from";

        original.reset();
        auto stillCalls = callback;
        stillCalls();
        EXPECT_EQ(calls, 1) << "neither the copy, the moved-to nor the assigned token keeps it alive";
        EXPECT_TRUE(copy.watch().lock());
        EXPECT_TRUE(moved.watch().lock());
    }

    TEST(LifetimeTokenTests, GuardedCallablesKeepTheirArguments)
    {
        const LifetimeToken token;
        std::string received;
        auto callback = token.guarded(
            [&received](std::string text, std::unique_ptr<int> number)
            {
                received = text + std::to_string(*number);
            }
        );
        callback(std::string{"value "}, std::make_unique<int>(7));
        EXPECT_EQ(received, "value 7");
    }
}
