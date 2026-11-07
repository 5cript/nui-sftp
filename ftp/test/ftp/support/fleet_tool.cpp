#include <ftp/support/server_fleet.hpp>

#include <fmt/format.h>

#include <cstdio>
#include <string_view>

/**
 * @brief Starts and stops the fleet for the ctest fixture: nui-sftp-ftp-fleet start|stop <endpoints file>.
 */
int main(int argc, char** argv)
{
    using namespace FileTransferProtocol::Test;

    if (argc != 3)
    {
        fmt::print(stderr, "Usage: {} start|stop <endpoints file>\n", argv[0]);
        return 2;
    }

    const std::string_view action{argv[1]};
    const auto result = action == "start" ? startFleet(argv[2]) : stopFleet(argv[2]);
    if (!result)
    {
        fmt::print(stderr, "{}\n", result.error());
        return 1;
    }
    return 0;
}
