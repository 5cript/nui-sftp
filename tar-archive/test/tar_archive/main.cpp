#include <utility/process_temporary_directory.hpp>
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
/** @brief The temporary directory of this process alone, so tests running in parallel share no files. */
std::filesystem::path temporaryRoot;

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    const Utility::ProcessTemporaryDirectory processTemporary{programDirectory / "temp"};
    temporaryRoot = processTemporary.path();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    return RUN_ALL_TESTS();
}
