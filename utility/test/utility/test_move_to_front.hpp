#pragma once

#include <utility/algorithm/move_to_front.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace Utility::Tests
{
    TEST(MoveToFrontTests, ElementMovesToTheFrontAndOthersKeepTheirOrder)
    {
        std::vector<int> values{1, 2, 3, 4};
        Algorithm::moveToFront(values, 2u);
        EXPECT_EQ(values, (std::vector<int>{3, 1, 2, 4}));
    }

    TEST(MoveToFrontTests, FirstElementStaysInPlace)
    {
        std::vector<int> values{1, 2, 3};
        Algorithm::moveToFront(values, 0u);
        EXPECT_EQ(values, (std::vector<int>{1, 2, 3}));
    }

    TEST(MoveToFrontTests, IndexOutOfRangeChangesNothing)
    {
        std::vector<int> values{1, 2, 3};
        Algorithm::moveToFront(values, 3u);
        EXPECT_EQ(values, (std::vector<int>{1, 2, 3}));

        std::vector<int> empty{};
        Algorithm::moveToFront(empty, 0u);
        EXPECT_TRUE(empty.empty());
    }
}
