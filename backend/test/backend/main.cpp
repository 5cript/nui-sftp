#include <utility/process_temporary_directory.hpp>
#include "nui_env.hpp"

#include "test_download_operation.hpp"
#include "test_upload_operation.hpp"
#include "test_scan_operation.hpp"
#include "test_local_delete_guard.hpp"
#include "test_local_delete_operation.hpp"
#include "test_local_removal.hpp"
#include "test_local_operation_queue.hpp"
#include "test_remote_delete_guard.hpp"
#include "test_local_scan_operation.hpp"
#include "test_bulk_download_operation.hpp"
#include "test_bulk_upload_operation.hpp"
#include "test_parallel_budget.hpp"
#include "test_archive_download_operation.hpp"
#include "test_archive_upload_operation.hpp"
#include "test_file_tracking.hpp"
#include "test_instance_bookkeeping.hpp"
#include "test_shell_integration.hpp"
#include "test_pseudo_terminal.hpp"
#include "test_ui_options.hpp"
#include "test_legacy_migration.hpp"
#include "test_terminal_renderer_migration.hpp"
#include "test_session_layouts.hpp"
#include "test_ssh_user_selection.hpp"
#include "test_environment.hpp"
#include "test_termios.hpp"
#include "test_fork_pool.hpp"
#include "test_file_change_filter.hpp"
#include "test_local_watch.hpp"
#include "test_settled_changes.hpp"
#include "test_tracking_state.hpp"
#include "test_password_request.hpp"
#include "test_sync_session.hpp"
#include "test_dropped_entry.hpp"
#include "test_transfer_failures.hpp"
#include "test_compared_state_check.hpp"
#include "test_operation_queue.hpp"
#include "test_existence_check.hpp"
#include "test_opener_risk.hpp"
#include "test_process.hpp"
#include "test_program_options.hpp"
#include "test_rpc_helper.hpp"
#include "test_rpc_path_parameters.hpp"

#include <log/log.hpp>

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;
/** @brief The temporary directory of this process alone, so tests running in parallel share no files. */
std::filesystem::path temporaryRoot;

int main(int argc, char** argv)
{
    Log::setLevel(Log::Level::Off);

    // Absolute path required: TemporaryDirectoryInstance::addWatch joins
    // instanceDir with the passed path when it isn't absolute, so a relative
    // instanceDir (from a relative programDirectory) doubles the path.
    programDirectory = std::filesystem::weakly_canonical(std::filesystem::path{argv[0]}.parent_path());

    const Utility::ProcessTemporaryDirectory processTemporary{programDirectory / "temp"};
    temporaryRoot = processTemporary.path();

    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new Test::NuiEnvGuard{});
    return RUN_ALL_TESTS();
}