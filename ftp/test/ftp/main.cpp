#include <utility/process_temporary_directory.hpp>
#include "test_command.hpp"
#include "test_control_connection.hpp"
#include "test_control_connection_tls.hpp"
#include "test_data_connection.hpp"
#include "test_download.hpp"
#include "test_upload.hpp"
#include "test_listing_transfer.hpp"
#include "test_metadata.hpp"
#include "test_features.hpp"
#include "test_listing.hpp"
#include "test_login.hpp"
#include "test_login_conformance.hpp"
#include "test_pool.hpp"
#include "test_pool_conformance.hpp"
#include "test_engine.hpp"
#include "test_ftp_file_system.hpp"
#include "test_delivery_ring.hpp"
#include "test_supply_ring.hpp"
#include "test_transfer_conformance.hpp"
#include "test_metadata_conformance.hpp"
#include "test_printable.hpp"
#include "test_reply_code.hpp"
#include "test_reply_error.hpp"
#include "test_reply_framer.hpp"
#include "test_reply_parsers.hpp"
#include "test_resolver.hpp"
#include "test_scripted_ftp_server.hpp"
#include "test_server_profiles.hpp"
#include "test_server_smoke.hpp"
#include "test_socket_options.hpp"
#include "test_tls.hpp"

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
