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

    TEST(EnqueueMinimizerTests, BulkDirInSetEmitsBulk)
    {
        // Sparse model: caller put the bulk dir itself in the set → emit bulk,
        // descendants covered.
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = true},
            {.relKey = "dir/a.txt", .isBulkDir = false},
            {.relKey = "dir/b.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"dir"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u) << "must emit the bulk dir, not its descendants";
    }

    TEST(EnqueueMinimizerTests, IndividualLeafSelectionEmitsLeavesEvenUnderBulkDir)
    {
        // Sparse model: caller did NOT collapse to the bulk dir; they selected
        // one file under it.  Minimizer just emits what's in the set.
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

    TEST(EnqueueMinimizerTests, OuterBulkDirCoversNestedBulkAndLeaves)
    {
        // outer is in the sparse set → bulk-emit and cover every descendant.
        std::vector<MinimizerItemView> items{
            {.relKey = "outer", .isBulkDir = true},
            {.relKey = "outer/inner", .isBulkDir = true},
            {.relKey = "outer/inner/leaf.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"outer"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u) << "outer bulk emission must cover the whole subtree";
    }

    TEST(EnqueueMinimizerTests, NonBulkIntermediateNodeIsNotEmittedItself)
    {
        // Two-sided structural directory — no SFTP primitive syncs it as one op.
        // Even when explicitly in the set it must not emit; its descendants do.
        std::vector<MinimizerItemView> items{
            {.relKey = "dir", .isBulkDir = false},
            {.relKey = "dir/leaf.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"dir/leaf.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 1u) << "intermediate non-bulk node must not be emitted";
    }

    TEST(EnqueueMinimizerTests, StructuralDirInSetExpandsToLeafDescendants)
    {
        // Sparse set contains a structural (two-sided) dir.  No bulk primitive
        // applies; every leaf descendant must emit individually via ancestor
        // implication.
        std::vector<MinimizerItemView> items{
            {.relKey = "parent", .isBulkDir = false},
            {.relKey = "parent/a.txt", .isBulkDir = false},
            {.relKey = "parent/sub", .isBulkDir = false},
            {.relKey = "parent/sub/b.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"parent"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_FALSE(resultContains(result, 0u)) << "structural dir itself not emitted";
        EXPECT_TRUE(resultContains(result, 1u));
        EXPECT_FALSE(resultContains(result, 2u)) << "nested structural dir not emitted";
        EXPECT_TRUE(resultContains(result, 3u));
    }

    TEST(EnqueueMinimizerTests, StructuralDirInSetStillCollapsesNestedBulk)
    {
        // Structural outer dir in the sparse set; contains a nested bulk dir.
        // The bulk dir should still collapse into a single emission (not
        // descend into its own children).
        std::vector<MinimizerItemView> items{
            {.relKey = "outer", .isBulkDir = false},
            {.relKey = "outer/bulk", .isBulkDir = true},
            {.relKey = "outer/bulk/x.txt", .isBulkDir = false},
            {.relKey = "outer/leaf.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"outer"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_FALSE(resultContains(result, 0u));
        EXPECT_TRUE(resultContains(result, 1u)) << "inner bulk dir emitted once";
        EXPECT_FALSE(resultContains(result, 2u)) << "leaf under inner bulk is covered";
        EXPECT_TRUE(resultContains(result, 3u));
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
        // Sparse: "left" means "take all of left"; individual leaves under
        // "right" are selected atomically.
        std::unordered_set<std::string> selected{"left", "right/b.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_TRUE(resultContains(result, 0u)) << "left bulk dir";
        EXPECT_FALSE(resultContains(result, 1u)) << "left/a.txt covered by bulk";
        EXPECT_FALSE(resultContains(result, 2u)) << "right not in set and no leaves cover it";
        EXPECT_TRUE(resultContains(result, 3u));
        EXPECT_FALSE(resultContains(result, 4u));
    }

    TEST(EnqueueMinimizerTests, BulkDirWithoutDescendantsIsEmittedWhenSelected)
    {
        // A directory item with no descendants in the diff list (empty dir
        // that needs to be created on the other side).  Selecting it must
        // cause the row to emit so the dir actually gets created.
        std::vector<MinimizerItemView> items{
            {.relKey = "empty_dir", .isBulkDir = true},
        };

        std::unordered_set<std::string> selected{"empty_dir"};
        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u);

        EXPECT_TRUE(minimizeEnqueueIndices(items, {}).empty());
    }

    TEST(EnqueueMinimizerTests, RedundantAncestorAndDescendantAreTolerated)
    {
        // Non-minimal sparse input: the ancestor already implies the descendant.
        // Minimizer should still produce the correct emission (bulk once).
        std::vector<MinimizerItemView> items{
            {.relKey = "outer", .isBulkDir = true},
            {.relKey = "outer/inner.txt", .isBulkDir = false},
        };
        std::unordered_set<std::string> selected{"outer", "outer/inner.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result.front(), 0u);
    }

    TEST(EnqueueMinimizerTests, PartialStructuralSubtreeEmitsOnlySelectedLeaves)
    {
        std::vector<MinimizerItemView> items{
            {.relKey = "parent", .isBulkDir = false},
            {.relKey = "parent/a.txt", .isBulkDir = false},
            {.relKey = "parent/b.txt", .isBulkDir = false},
            {.relKey = "parent/c.txt", .isBulkDir = false},
        };
        // Sparse leaf-level selection (user filled out after unchecking one).
        std::unordered_set<std::string> selected{"parent/a.txt", "parent/c.txt"};

        const auto result = minimizeEnqueueIndices(items, selected);
        EXPECT_FALSE(resultContains(result, 0u));
        EXPECT_TRUE(resultContains(result, 1u));
        EXPECT_FALSE(resultContains(result, 2u));
        EXPECT_TRUE(resultContains(result, 3u));
    }
}
