#include "test_path_segment.hpp"
#include "test_remote_path.hpp"
#include "test_local_path.hpp"
#include "test_native.hpp"
#include "test_utf8.hpp"

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
