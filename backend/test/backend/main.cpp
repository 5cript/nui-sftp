// Broken: needs complete rewrite:
// #include "test_download_operation.hpp"
#include "test_upload_operation.hpp"

#include <log/log.hpp>

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    Log::setLevel(Log::Level::Off);

    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}