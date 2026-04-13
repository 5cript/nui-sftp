#pragma once

#include <shared_data/sync/enqueue_minimizer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace SharedData::Sync::Test
{
    inline bool resultContains(std::vector<std::size_t> const& indices, std::size_t value)
    {
        return std::find(indices.begin(), indices.end(), value) != indices.end();
    }

    TEST(EnqueueMinimizerTests, EmptyInputProducesEmptyResult)
    {
        const auto result = minimizeEnqueueIndices({}, {});
        EXPECT_TRUE(result.empty());
    }

    TEST(EnqueueMinimizerTests, SingleSelectedLeafIsEmitted)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "file.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"file.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u);
    }

    TEST(EnqueueMinimizerTests, UnselectedLeafIsSkipped)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "file.txt", .isBulkDir = false},
        };

        const auto result = minimizeEnqueueIndices(items, {});
        EXPECT_TRUE(result.empty());
    }

    TEST(EnqueueMinimizerTests, FullySelectedSubtreeCollapsesToBulkDir)
    {
        // dir/ is a bulk dir; both descendants are selected → emit just the dir.
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = true},
            {.relKey = "dir/a.txt", .isBulkDir = false},
            {.relKey = "dir/b.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"dir/a.txt", "dir/b.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u) << "must emit the bulk dir, not its descendants";
    }

    TEST(EnqueueMinimizerTests, PartiallySelectedSubtreeEmitsOnlySelectedLeaves)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = true},
            {.relKey = "dir/a.txt", .isBulkDir = false},
            {.relKey = "dir/b.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"dir/a.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_FALSE(resultContains(result, 0u)) << "bulk dir must not be emitted on partial selection";
        EXPECT_TRUE(resultContains(result, 1u));
        EXPECT_FALSE(resultContains(result, 2u));
    }

    TEST(EnqueueMinimizerTests, EntirelyUnselectedSubtreeIsSkipped)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = true},
            {.relKey = "dir/a.txt", .isBulkDir = false},
            {.relKey = "dir/b.txt", .isBulkDir = false},
        };

        const auto result = minimizeEnqueueIndices(items, {});
        EXPECT_TRUE(result.empty());
    }

    TEST(EnqueueMinimizerTests, NestedFullySelectedSubtreeIsCoveredByOuterDir)
    {
        // outer/ contains inner/ which contains file. With everything selected,
        // only the outer bulk dir should be emitted (covers nested dir + leaf).
        std::vector<MinimizerItemView> items{
            {.relKey = "outer", .isBulkDir = true},
            {.relKey = "outer/inner", .isBulkDir = true},
            {.relKey = "outer/inner/leaf.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"outer/inner/leaf.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u) << "expected outer dir to cover nested subtree";
    }

    TEST(EnqueueMinimizerTests, NonBulkIntermediateNodeIsNotEmittedItself)
    {
        // "dir" is NOT a bulk dir (e.g., it represents a file action that happens to share
        // a relKey with descendants — the production scenario is rare but the algorithm
        // still must not emit it on its own).
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = false},
            {.relKey = "dir/leaf.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"dir/leaf.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 1u) << "intermediate non-bulk node must not be emitted";
    }

    TEST(EnqueueMinimizerTests, MixedSelectionAcrossSiblingSubtrees)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "left", .isBulkDir = true},
            {.relKey = "left/a.txt", .isBulkDir = false},
            {.relKey = "right", .isBulkDir = true},
            {.relKey = "right/b.txt", .isBulkDir = false},
            {.relKey = "right/c.txt", .isBulkDir = false},
        };
        // left is fully selected → collapse to bulk dir.
        // right is partially selected → emit b.txt only.
        std::unordered_set<std::string> selected{"left/a.txt", "right/b.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_TRUE(resultContains(result, 0u)) << "left bulk dir";
        EXPECT_FALSE(resultContains(result, 1u));
        EXPECT_FALSE(resultContains(result, 2u)) << "right not fully selected → no bulk";
        EXPECT_TRUE(resultContains(result, 3u));
        EXPECT_FALSE(resultContains(result, 4u));
    }

    TEST(EnqueueMinimizerTests, BulkDirWithoutDescendantsIsEmittedWhenSelected)
    {
        // A directory item with no descendants in the diff list (e.g., an empty dir
        // that needs to be created on the other side) is itself a tree leaf — selecting
        // it must cause it to be emitted so the dir actually gets created.
        std::vector<MinimizerItemView> items{
            {.relKey = "empty_dir", .isBulkDir = true},
        };

        std::unordered_set<std::string> selected{"empty_dir"};
        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u);

        // Without selection it is suppressed.
        EXPECT_TRUE(minimizeEnqueueIndices(items, {}).empty());
    }

    TEST(EnqueueMinimizerTests, OuterDirNotEmittedWhenInnerHasUnselectedLeaf)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "outer", .isBulkDir = true},
            {.relKey = "outer/a.txt", .isBulkDir = false},
            {.relKey = "outer/inner", .isBulkDir = true},
            {.relKey = "outer/inner/leaf.txt", .isBulkDir = false},
            {.relKey = "outer/inner/other.txt", .isBulkDir = false},
        };
        // Skip outer/inner/other.txt → outer cannot be collapsed.
        std::unordered_set<std::string> selected{"outer/a.txt", "outer/inner/leaf.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_FALSE(resultContains(result, 0u)) << "outer must not be emitted";
        EXPECT_FALSE(resultContains(result, 2u)) << "inner not fully selected either";
        EXPECT_TRUE(resultContains(result, 1u));
        EXPECT_TRUE(resultContains(result, 3u));
        EXPECT_FALSE(resultContains(result, 4u));
    }
}
