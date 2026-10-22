#pragma once

#include <frontend/session_components/row_order.hpp>

#include <gtest/gtest.h>

#include <string>

namespace FileTracking::Test
{
    TEST(RowOrderTests, GivesTheIndexOfARowWhenItIsRemoved)
    {
        RowOrder<std::string> rows;
        rows.add("first");
        rows.add("second");
        rows.add("third");

        EXPECT_EQ(rows.remove("second"), std::size_t{1});
        EXPECT_EQ(rows.remove("third"), std::size_t{1});
        EXPECT_EQ(rows.remove("first"), std::size_t{0});
        EXPECT_EQ(rows.size(), std::size_t{0});
    }

    TEST(RowOrderTests, KnowsNoRowOfAnUnknownOrRemovedKey)
    {
        RowOrder<std::string> rows;
        rows.add("only");

        EXPECT_FALSE(rows.remove("other").has_value());
        ASSERT_TRUE(rows.remove("only").has_value());
        EXPECT_FALSE(rows.remove("only").has_value());
    }
}
