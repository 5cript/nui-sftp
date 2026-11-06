#include <ftp/support/server_fleet.hpp>

#ifndef _WIN32
#    include <ftp/support/test_ftp_client.hpp>

#    include <docker/container.hpp>
#    include <docker/docker.hpp>
#endif

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#    include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        constexpr std::string_view serversDirectory = NUI_SFTP_FTP_SERVERS_DIRECTORY;
        constexpr std::string_view serverList = NUI_SFTP_FTP_TEST_SERVERS;
    }

    std::vector<std::string> selectedServers()
    {
        std::vector<std::string> names{};
        for (auto const name : serverList | std::views::split(','))
            names.emplace_back(std::string_view{name});
        return names;
    }

    std::expected<ServerProfile, std::string> serverProfile(std::string_view server)
    {
        return loadServerProfile(std::filesystem::path{serversDirectory} / server / "profile.json");
    }

#ifndef _WIN32
    namespace
    {
        using nlohmann::json;

        constexpr std::chrono::seconds readyWait{20};
        constexpr std::string_view fixturesPath = "/srv/ftptest/fixtures";
        constexpr std::string_view workspaceFormat = "/srv/ftptest/t-{}-{}";

        /**
         * @brief What a run has of the fleet. Containers are only set when this process started them.
         */
        struct Fleet
        {
            std::optional<std::string> unavailable{};
            std::vector<ServerEndpoint> endpoints{};
            std::vector<Docker::Container> containers{};
        };

        /**
         * @brief The docker network of this process when it runs in a container itself, as in CI: a sibling container
         *        is reachable only on the same network. Empty when it runs on the host.
         */
        std::vector<std::string> ownNetworkArguments()
        {
            std::array<char, 256u> hostName{};
            if (!std::filesystem::exists("/.dockerenv") || gethostname(hostName.data(), hostName.size()) != 0)
                return {};
            const auto inspect = Docker::run(fmt::format(
                "docker inspect --format "
                "'{{{{range $name, $network := .NetworkSettings.Networks}}}}{{{{$name}}}} {{{{end}}}}' {}",
                Docker::quote(hostName.data())
            ));
            if (inspect.exitCode != 0)
                return {};
            auto network = inspect.output.substr(0u, inspect.output.find_first_of(" \r\n"));
            if (network.empty())
                return {};
            return {"--network", std::move(network)};
        }

        /**
         * @brief Polls the greeting, a starting server gives no other sign.
         */
        std::expected<void, std::string> waitUntilReady(std::string const& address, std::uint16_t port)
        {
            const auto deadline = std::chrono::steady_clock::now() + readyWait;
            std::string lastProblem{};
            while (std::chrono::steady_clock::now() < deadline)
            {
                TestFtpClient client{std::chrono::seconds{2}};
                const auto greeting = client.connect(address, port).and_then(
                    [&client]()
                    {
                        return client.readReply();
                    }
                );
                if (greeting && greeting->code == 220)
                    return {};
                lastProblem = greeting ? greeting->text : greeting.error();
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            return std::unexpected(fmt::format("{}:{} is not ready: {}", address, port, lastProblem));
        }

        std::expected<void, std::string>
        startVariant(ServerProfile const& profile, std::string const& name, ServerVariant const& variant, Fleet& fleet)
        {
            auto arguments = ownNetworkArguments();
            arguments.insert(arguments.end(), profile.runArguments.begin(), profile.runArguments.end());
            arguments.insert(arguments.end(), variant.runArguments.begin(), variant.runArguments.end());

            auto container = Docker::Container::start({
                .image = profile.image,
                .namePrefix = fmt::format("nui-sftp-ftp-{}-{}", profile.name, name),
                .runArguments = std::move(arguments),
                .command = variant.command,
            });
            if (!container)
                return std::unexpected(std::move(container).error());

            const auto address = container->ipAddress();
            if (!address)
                return std::unexpected(address.error());
            fleet.endpoints.push_back({
                .server = profile.name,
                .variant = name,
                .container = container->name(),
                .address = *address,
            });
            fleet.containers.push_back(std::move(container).value());
            return waitUntilReady(*address, profile.plainPort);
        }

        /**
         * @brief Starts everything. Containers started so far go away with @p fleet when one fails.
         */
        std::expected<void, std::string> startContainers(Fleet& fleet)
        {
            if (const auto available = Docker::checkAvailable(); !available)
            {
                fleet.unavailable = fmt::format("{}. The conformance suites run servers in containers.", available.error());
                return {};
            }

            for (auto const& server : selectedServers())
            {
                const auto profile = serverProfile(server);
                if (!profile)
                    return std::unexpected(profile.error());

                const auto built = Docker::buildImage(
                    profile->image, profile->directory / "Dockerfile", profile->directory.parent_path()
                );
                if (!built)
                    return std::unexpected(built.error());

                for (auto const& [name, variant] : profile->variants)
                {
                    if (const auto started = startVariant(*profile, name, variant, fleet); !started)
                        return std::unexpected(started.error());
                }
            }
            return {};
        }

        std::expected<Fleet, std::string> readEndpoints(std::filesystem::path const& file)
        {
            std::ifstream stream{file};
            if (!stream)
                return std::unexpected(fmt::format("Cannot read {}", file.generic_string()));
            try
            {
                const auto document = json::parse(stream);
                Fleet fleet{};
                if (document.contains("unavailable"))
                    fleet.unavailable = document.at("unavailable").get<std::string>();
                for (auto const& endpoint : document.at("endpoints"))
                {
                    fleet.endpoints.push_back({
                        .server = endpoint.at("server").get<std::string>(),
                        .variant = endpoint.at("variant").get<std::string>(),
                        .container = endpoint.at("container").get<std::string>(),
                        .address = endpoint.at("address").get<std::string>(),
                    });
                }
                return fleet;
            }
            catch (json::exception const& error)
            {
                return std::unexpected(fmt::format("{}: {}", file.generic_string(), error.what()));
            }
        }

        std::expected<void, std::string> writeEndpoints(std::filesystem::path const& file, Fleet const& fleet)
        {
            json endpoints = json::array();
            for (auto const& endpoint : fleet.endpoints)
            {
                endpoints.push_back({
                    {"server", endpoint.server},
                    {"variant", endpoint.variant},
                    {"container", endpoint.container},
                    {"address", endpoint.address},
                });
            }
            json document{{"endpoints", std::move(endpoints)}};
            if (fleet.unavailable)
                document["unavailable"] = *fleet.unavailable;

            std::filesystem::create_directories(file.parent_path());
            std::ofstream stream{file};
            stream << document.dump(4);
            if (!stream)
                return std::unexpected(fmt::format("Cannot write {}", file.generic_string()));
            return {};
        }

        std::expected<Fleet, std::string> findOrStartFleet()
        {
            if (char const* file = std::getenv("NUI_SFTP_FTP_FLEET_ENDPOINTS"); file != nullptr)
                return readEndpoints(file);

            Fleet fleet{};
            if (const auto started = startContainers(fleet); !started)
                return std::unexpected(started.error());
            return fleet;
        }

        std::expected<Fleet, std::string> const& fleet()
        {
            static const std::expected<Fleet, std::string> instance = findOrStartFleet();
            return instance;
        }
    }

    std::optional<std::string> const& fleetUnavailable()
    {
        static const std::optional<std::string> none{};
        return fleet() ? fleet()->unavailable : none;
    }

    std::expected<ServerEndpoint, std::string> serverEndpoint(std::string_view server, std::string_view variant)
    {
        if (!fleet())
            return std::unexpected(fleet().error());
        const auto found = std::ranges::find_if(
            fleet()->endpoints,
            [&](ServerEndpoint const& endpoint)
            {
                return endpoint.server == server && endpoint.variant == variant;
            }
        );
        if (found == fleet()->endpoints.end())
            return std::unexpected(fmt::format("The fleet has no {} {}", server, variant));
        return *found;
    }

    std::expected<std::string, std::string> prepareWorkspace(ServerEndpoint const& endpoint)
    {
        static std::atomic<int> nextWorkspace{0};
        const auto workspace = fmt::format(fmt::runtime(workspaceFormat), getpid(), ++nextWorkspace);
        const auto copied = Docker::executeIn(
            endpoint.container, fmt::format("cp -a {} {}", fixturesPath, Docker::quote(workspace))
        );
        if (copied.exitCode != 0)
            return std::unexpected(fmt::format("Copying the fixtures on {}: {}", endpoint.server, copied.output));
        return workspace;
    }

    std::expected<void, std::string> startFleet(std::filesystem::path const& file)
    {
        // A run that stopped halfway left its containers behind.
        if (std::filesystem::exists(file))
        {
            if (const auto stopped = stopFleet(file); !stopped)
                return stopped;
        }

        Fleet started{};
        if (const auto result = startContainers(started); !result)
            return result;
        const auto written = writeEndpoints(file, started);
        if (!written)
            return written;
        for (auto& container : started.containers)
            std::move(container).release();
        return {};
    }

    std::expected<void, std::string> stopFleet(std::filesystem::path const& file)
    {
        const auto running = readEndpoints(file);
        if (!running)
            return std::unexpected(running.error());
        for (auto const& endpoint : running->endpoints)
            Docker::removeContainer(endpoint.container);
        std::filesystem::remove(file);
        return {};
    }
#endif
}
