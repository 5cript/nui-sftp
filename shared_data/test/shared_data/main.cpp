#include "test_bulk_delete_request.hpp"
#include "test_bulk_transfer_request.hpp"
#include "test_compared_state.hpp"
#include "test_operation_added.hpp"
#include "test_operation_error.hpp"
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
