#pragma once

#include <backend/rpc_helper.hpp>
#include "nui_env.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>

namespace Test
{
    TEST(RpcHelperTests, ArgumentsOfTheSecondTryAreValidUtf8)
    {
        const auto text = RpcHelper::Detail::withValidUtf8Argument(std::string{"a\xFF"});
        EXPECT_EQ(text, nlohmann::json("a\xEF\xBF\xBD"));

        const std::uint64_t large = 1ull << 60u;
        const auto number = RpcHelper::Detail::withValidUtf8Argument(large);
        static_assert(std::is_same_v<std::decay_t<decltype(number)>, std::uint64_t>);
        EXPECT_EQ(number, large);
    }

    TEST(RpcHelperTests, CallsCarryingTextThatIsNotUtf8NeverThrow)
    {
        if (!NuiEnv::instance().available())
            GTEST_SKIP() << "No display available";
        auto& hub = *NuiEnv::instance().hub;
        const nlohmann::json invalid{{"name", "Fran\xE7ois"}};
        EXPECT_THROW(hub.callRemote("RpcHelperTests::nobody", invalid), nlohmann::json::type_error);
        RpcHelper::callRemoteSafely(hub, "RpcHelperTests::nobody", invalid);
        RpcHelper::callRemoteSafely(hub, "RpcHelperTests::nobody", std::string{"Fran\xE7ois"}, 42, true);
        RpcHelper::callRemoteSafely(hub, "RpcHelperTests::nobody", {{"error", "Fran\xE7ois"}});
    }
}
