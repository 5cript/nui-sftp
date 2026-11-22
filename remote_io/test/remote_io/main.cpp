#include "test_buffer_provider.hpp"
#include "test_processing_thread.hpp"
#include "test_remote_error.hpp"
#include "test_remote_file_system.hpp"
#include "test_remote_file_system_helpers.hpp"
#include "test_transfer_context.hpp"

#include <gtest/gtest.h>

#include <vector>

int main(int argc, char** argv)
{
    // GoogleTest removes its flags from the arguments it gets. ThreadSanitizer finds the environment it hands
    // llvm-symbolizer behind the end of the original ones, which stay untouched for it.
    std::vector<char*> arguments{argv, argv + argc + 1};
    ::testing::InitGoogleTest(&argc, arguments.data());
    return RUN_ALL_TESTS();
}
