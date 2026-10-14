#include <utility/process_temporary_directory.hpp>
#include "test_buffer_provider.hpp"
#include "test_processing_thread.hpp"
#include "test_transfer_chunk_controller.hpp"
#include "test_ssh_session.hpp"
#include "test_sftp.hpp"
#include "test_sftp_in_strand.hpp"
#include "test_key_file.hpp"

#include <utility/node/node.hpp>

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