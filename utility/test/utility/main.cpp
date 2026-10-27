#include <utility/process_temporary_directory.hpp>
#include "test_case_convert.hpp"
#include "test_checked_symlink_status.hpp"
#include "test_command_template.hpp"
#include "test_directory_traversal.hpp"
#include "test_download_mark.hpp"
#include "test_echo_suppressor.hpp"
#include "test_error_message.hpp"
#include "test_extended_length_path.hpp"
#include "test_fuzzy_search.hpp"
#include "test_json_utf8.hpp"
#include "test_keyed_diff.hpp"
#include "test_lifetime_token.hpp"
#include "test_localized_message.hpp"
#include "test_move_to_front.hpp"
#include "test_permissions.hpp"
#include "test_rename.hpp"
#include "test_resource_root.hpp"
#include "test_link_copy.hpp"
#include "test_link_safe_remove.hpp"
#include "test_shared_input_file_stream.hpp"
#include "test_shell_integration.hpp"
#include "test_typed_line_buffer.hpp"
#include "test_user_directories.hpp"
#include "test_utf8.hpp"

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
    return RUN_ALL_TESTS();
}