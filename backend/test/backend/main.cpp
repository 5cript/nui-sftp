#include "nui_env.hpp"

#include "test_download_operation.hpp"
#include "test_upload_operation.hpp"
#include "test_scan_operation.hpp"
#include "test_local_scan_operation.hpp"
#include "test_bulk_download_operation.hpp"
#include "test_bulk_upload_operation.hpp"
#include "test_parallel_budget.hpp"
#include "test_archive_download_operation.hpp"
#include "test_archive_upload_operation.hpp"
#include "test_file_tracking.hpp"
#include "test_shell_integration.hpp"
#include "test_ui_options.hpp"
#include "test_environment.hpp"
#include "test_termios.hpp"
#include "test_fork_pool.hpp"

#include <log/log.hpp>

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    Log::setLevel(Log::Level::Off);

    // Absolute path required: TemporaryDirectoryInstance::addWatch joins
    // instanceDir with the passed path when it isn't absolute, so a relative
    // instanceDir (from a relative programDirectory) doubles the path.
    programDirectory = std::filesystem::weakly_canonical(std::filesystem::path{argv[0]}.parent_path());

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new Test::NuiEnvGuard{});
    return RUN_ALL_TESTS();
}