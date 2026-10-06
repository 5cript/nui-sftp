#include "test_command_template.hpp"
#include "test_directory_traversal.hpp"
#include "test_echo_suppressor.hpp"
#include "test_keyed_diff.hpp"
#include "test_localized_message.hpp"
#include "test_shell_integration.hpp"
#include "test_typed_line_buffer.hpp"

#include <gtest/gtest.h>

#include <filesystem>

std::filesystem::path programDirectory;

int main(int argc, char** argv)
{
    programDirectory = std::filesystem::path{argv[0]}.parent_path();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}