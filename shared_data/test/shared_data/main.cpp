#include "test_diff.hpp"
#include "test_diff_tree_store.hpp"
#include "test_enqueue_minimizer.hpp"
#include "test_tree_directory_walker.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
