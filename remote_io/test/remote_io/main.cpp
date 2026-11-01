#include "test_buffer_provider.hpp"
#include "test_processing_thread.hpp"
#include "test_remote_error.hpp"
#include "test_transfer_context.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
