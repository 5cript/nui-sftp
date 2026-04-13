#include "nui_env.hpp"

#include "test_download_operation.hpp"
#include "test_upload_operation.hpp"
#include "test_scan_operation.hpp"
#include "test_local_scan_operation.hpp"
#include "test_bulk_download_operation.hpp"
#include "test_bulk_upload_operation.hpp"
#include "test_file_tracking.hpp"

#include <log/log.hpp>

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    Log::setLevel(Log::Level::Off);

    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new Test::NuiEnvGuard{});
    return RUN_ALL_TESTS();
}