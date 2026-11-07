#pragma once

#include <ftp/support/server_profile.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief The servers selected by NUI_SFTP_FTP_TEST_SERVERS, from their directories below servers/.
     *
     * @return The names of the servers, what the conformance suites run over.
     */
    std::vector<std::string> selectedServers();

    /**
     * @brief The profile of a selected server.
     */
    std::expected<ServerProfile, std::string> serverProfile(std::string_view server);

#ifndef _WIN32
    /**
     * @brief One running variant of a server.
     */
    struct ServerEndpoint
    {
        std::string server{};
        std::string variant{};
        std::string container{};
        std::string address{};
    };

    /**
     * @brief Why the suites against real servers cannot run here, the reason they skip with.
     *
     * @return Nothing when they can. Asking starts the fleet when no ctest fixture did.
     */
    std::optional<std::string> const& fleetUnavailable();

    /**
     * @brief Finds a running server. Under ctest a fixture started the fleet once and left its endpoints in the file
     *        named by NUI_SFTP_FTP_FLEET_ENDPOINTS, otherwise this process starts it on first use.
     *
     * @return The endpoint, or why there is none.
     */
    std::expected<ServerEndpoint, std::string>
    serverEndpoint(std::string_view server, std::string_view variant = "default");

    /**
     * @brief Copies the fixture tree to a directory for one test alone, so tests run in parallel on one container.
     *
     * @return The absolute path of that directory on the server.
     */
    std::expected<std::string, std::string> prepareWorkspace(ServerEndpoint const& endpoint);

    /**
     * @brief Builds the images, starts every variant of every selected server and writes the endpoints to @p file.
     *        The containers keep running until stopFleet(). Without docker the file says why the suites skip.
     */
    std::expected<void, std::string> startFleet(std::filesystem::path const& file);

    /**
     * @brief Removes the containers named in @p file, and the file.
     */
    std::expected<void, std::string> stopFleet(std::filesystem::path const& file);
#endif
}
