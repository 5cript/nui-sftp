#pragma once

#include <utility/json_utf8.hpp>

#include <gtest/gtest.h>

#include <string>
#include <tuple>

namespace Utility::Test
{
    TEST(JsonUtf8Tests, StringsAndKeysThatAreNotUtf8BecomeDumpable)
    {
        const nlohmann::json json{
            {"name", "a\xFF"},
            {"list", {"ok", "b\xC3"}},
            {"nested", {{"c\xFE", 1}}},
            {"number", 42},
        };
        EXPECT_THROW(std::ignore = json.dump(), nlohmann::json::type_error);

        const auto valid = withValidUtf8(json);
        EXPECT_NO_THROW(std::ignore = valid.dump());
        EXPECT_EQ(valid["name"], "a\xEF\xBF\xBD");
        EXPECT_EQ(valid["list"][0], "ok");
        EXPECT_EQ(valid["list"][1], "b\xEF\xBF\xBD");
        EXPECT_EQ(valid["nested"]["c\xEF\xBF\xBD"], 1);
        EXPECT_EQ(valid["number"], 42);
    }

    TEST(JsonUtf8Tests, ValidJsonStaysTheSame)
    {
        const nlohmann::json json{{"name", "Grüße"}, {"values", {1, 2, 3}}, {"flag", true}};
        EXPECT_EQ(withValidUtf8(json), json);
    }
}
