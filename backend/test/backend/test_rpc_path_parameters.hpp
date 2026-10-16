#pragma once

#include <backend/rpc_helper.hpp>
#include <backend/rpc_path_parameters.hpp>
#include <shared_data/file_operations/bulk_delete_request.hpp>
#include "nui_env.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace Test
{
    TEST(RpcPathParametersTests, AbsoluteRemotePathsParse)
    {
        const auto parsed = RpcHelper::parseRemotePath("/home/test/a.txt");
        ASSERT_TRUE(parsed.has_value()) << parsed.error();
        EXPECT_EQ(parsed->value(), "/home/test/a.txt");
    }

    TEST(RpcPathParametersTests, AnInvalidRemotePathIsNamedInTheMessage)
    {
        for (auto const& invalid : {std::string{"relative/a.txt"}, std::string{}, std::string{"/a/../b"}})
        {
            const auto parsed = RpcHelper::parseRemotePath(invalid);
            ASSERT_FALSE(parsed.has_value()) << invalid;
            EXPECT_TRUE(parsed.error().starts_with("Invalid remote path: ")) << parsed.error();
        }
    }

    TEST(RpcPathParametersTests, RemotePathsStopAtTheFirstInvalidOne)
    {
        const auto valid = RpcHelper::parseRemotePaths({"/a", "/b/c"});
        ASSERT_TRUE(valid.has_value()) << valid.error();
        ASSERT_EQ(valid->size(), 2u);
        EXPECT_EQ((*valid)[1].value(), "/b/c");

        const auto invalid = RpcHelper::parseRemotePaths({"/a", "b", "/c"});
        ASSERT_FALSE(invalid.has_value());
        EXPECT_TRUE(invalid.error().starts_with("Invalid remote path: ")) << invalid.error();

        const auto none = RpcHelper::parseRemotePaths({});
        ASSERT_TRUE(none.has_value());
        EXPECT_TRUE(none->empty());
    }

    TEST(RpcPathParametersTests, LocalPathsMustBeAbsolute)
    {
#ifdef _WIN32
        const std::string absolute = "C:/Users/test/a.txt";
#else
        const std::string absolute = "/home/test/a.txt";
#endif
        const auto parsed = RpcHelper::parseLocalPath(absolute);
        ASSERT_TRUE(parsed.has_value()) << parsed.error();
        EXPECT_EQ(parsed->toUtf8(), absolute);

        const auto relative = RpcHelper::parseLocalPath("relative/a.txt");
        ASSERT_FALSE(relative.has_value());
        EXPECT_TRUE(relative.error().starts_with("Invalid local path: ")) << relative.error();

        const auto all = RpcHelper::parseLocalPaths({absolute, "relative"});
        ASSERT_FALSE(all.has_value());
        EXPECT_TRUE(all.error().starts_with("Invalid local path: ")) << all.error();
    }

    TEST(RpcPathParametersTests, ARequestThatDoesNotParseNamesWhatItIs)
    {
        const auto invalidTarget =
            nlohmann::json{{"entries", {{{"target", "relative"}, {"isDirectory", false}, {"sizeBytes", 0}}}}};
        const auto parsed = RpcHelper::parseRequest<SharedData::BulkDeleteRequest>(invalidTarget, "bulk delete");
        ASSERT_FALSE(parsed.has_value());
        EXPECT_TRUE(parsed.error().starts_with("Invalid bulk delete: ")) << parsed.error();

        const auto notJson = RpcHelper::parseRequest<std::vector<std::string>>(
            nlohmann::json::parse("{not json", nullptr, false), "archive roots"
        );
        ASSERT_FALSE(notJson.has_value());
        EXPECT_TRUE(notJson.error().starts_with("Invalid archive roots: ")) << notJson.error();
    }

    TEST(RpcPathParametersTests, AMovedReplyCountsAsAnswered)
    {
        if (!NuiEnv::instance().available())
            GTEST_SKIP() << "No display available";
        auto& environment = NuiEnv::instance();
        RpcHelper::RpcOnce original{*environment.window, *environment.hub, "RpcPathParametersTests::reply"};
        EXPECT_FALSE(original.answered());

        RpcHelper::RpcOnce moved{std::move(original)};
        EXPECT_TRUE(original.answered());
        EXPECT_FALSE(moved.answered());

        RpcHelper::RpcOnce assigned{*environment.window, *environment.hub, "RpcPathParametersTests::other"};
        assigned = std::move(moved);
        EXPECT_TRUE(moved.answered());
        EXPECT_FALSE(assigned.answered());
    }
}
