#include "test_header.hpp"
#include "test_compression.hpp"
#include "test_roundtrip.hpp"
#include "test_pax.hpp"
#include "test_misuse.hpp"
#include "test_size_boundaries.hpp"
#include "test_gnu_interop.hpp"

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    return RUN_ALL_TESTS();
}
