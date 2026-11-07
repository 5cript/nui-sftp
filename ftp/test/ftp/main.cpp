#include <utility/process_temporary_directory.hpp>
#include "test_command.hpp"
#include "test_reply_code.hpp"
#include "test_reply_error.hpp"
#include "test_reply_framer.hpp"
#include "test_scripted_ftp_server.hpp"
#include "test_server_profiles.hpp"
#include "test_server_smoke.hpp"

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
