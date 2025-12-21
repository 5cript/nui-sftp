#include "test_processing_thread.hpp"
#include "test_ssh_session.hpp"
#include "test_sftp.hpp"

#include <utility/node/node.hpp>

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}