#include <utility/process_temporary_directory.hpp>
#include "test_transfer_chunk_controller.hpp"
#include "test_request_window.hpp"
#include "test_ssh_session.hpp"
#include "test_sftp.hpp"
#include "test_sftp_in_strand.hpp"
#include "test_sftp_file_system.hpp"
#include "test_key_file.hpp"
#include "test_sftp_error_conversion.hpp"

#include <utility/node/node.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

std::filesystem::path programDirectory;
/** @brief The temporary directory of this process alone, so tests running in parallel share no files. */
std::filesystem::path temporaryRoot;

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    const Utility::ProcessTemporaryDirectory processTemporary{programDirectory / "temp"};
    temporaryRoot = processTemporary.path();

    // GoogleTest removes its flags from the arguments it gets. ThreadSanitizer finds the environment it hands
    // llvm-symbolizer behind the end of the original ones, which stay untouched for it.
    std::vector<char*> arguments{argv, argv + argc + 1};
    ::testing::InitGoogleTest(&argc, arguments.data());
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    return RUN_ALL_TESTS();
}