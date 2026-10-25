#include "application_client.hpp"
#include "connection.hpp"
#include "libssh_client.hpp"
#include "openssh_client.hpp"
#include "openssh_server.hpp"
#include "report.hpp"
#include "workload.hpp"

#include <docker/docker.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/process_temporary_directory.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <libssh/libssh.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Benchmark;

namespace
{
    constexpr std::uint64_t kibibyte = 1024u;
    constexpr std::uint64_t mebibyte = 1024u * kibibyte;

    struct Arguments
    {
        std::filesystem::path output{};
        int repeats{3};
        std::uint64_t largeMebibytes{256};
        std::uint64_t mediumCount{64};
        std::uint64_t mediumKibibytes{1024};
        std::uint64_t smallCount{2000};
        std::uint64_t smallKibibytes{4};
    };

    constexpr std::string_view usage =
        "nui-sftp-transfer-benchmark [options]\n"
        "  Transfers files between this host and OpenSSH in a docker container and writes a summary.\n"
        "  --output <file>       Where the summary goes (default: transfer-benchmark.md next to this program)\n"
        "  --repeat <n>          Runs per client and workload (default: 3)\n"
        "  --large-mib <n>       Size of the single large file (default: 256)\n"
        "  --medium-count <n>    Number of medium files (default: 64)\n"
        "  --medium-kib <n>      Size of a medium file (default: 1024)\n"
        "  --small-count <n>     Number of small files (default: 2000)\n"
        "  --small-kib <n>       Size of a small file (default: 4)\n";

    std::optional<std::uint64_t> parseNumber(std::string_view text)
    {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0)
            return std::nullopt;
        return value;
    }

    std::expected<Arguments, std::string> parseArguments(int argc, char** argv, std::filesystem::path programDirectory)
    {
        Arguments arguments{.output = programDirectory / "transfer-benchmark.md"};
        const std::map<std::string_view, std::uint64_t*> numbers{
            {"--large-mib", &arguments.largeMebibytes},
            {"--medium-count", &arguments.mediumCount},
            {"--medium-kib", &arguments.mediumKibibytes},
            {"--small-count", &arguments.smallCount},
            {"--small-kib", &arguments.smallKibibytes},
        };
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view option{argv[index]};
            if (option == "--help" || option == "-h")
                return std::unexpected{std::string{usage}};
            if (index + 1 >= argc)
                return std::unexpected{fmt::format("{} needs a value\n\n{}", option, usage)};
            const std::string_view value{argv[++index]};

            if (option == "--output")
            {
                arguments.output = std::filesystem::absolute(value);
                continue;
            }
            const auto number = parseNumber(value);
            if (!number)
                return std::unexpected{fmt::format("{} needs a positive number, not {}\n", option, value)};
            if (option == "--repeat")
                arguments.repeats = static_cast<int>(*number);
            else if (const auto target = numbers.find(option); target != numbers.end())
                *target->second = *number;
            else
                return std::unexpected{fmt::format("unknown option {}\n\n{}", option, usage)};
        }
        return arguments;
    }

    /**
     * @brief The source of a workload on both sides, which every client moves to its target.
     */
    struct Source
    {
        Workload workload{};
        std::filesystem::path localPath{};
        std::string remotePath{};
        std::vector<std::string> localFiles{};
        std::vector<std::string> remoteFiles{};
        std::string localDigest{};
        std::string remoteDigest{};
    };

    std::string remoteJoin(std::string_view root, std::string_view name)
    {
        return fmt::format("{}/{}", root, name);
    }

    std::filesystem::path localTargetRoot(std::filesystem::path const& workDirectory)
    {
        return workDirectory / "target";
    }

    std::string remoteTargetRoot()
    {
        return remoteJoin(OpenSshServer::dataDirectory, "target");
    }

    std::expected<Source, std::string>
    prepareSource(Workload const& workload, OpenSshServer const& server, std::filesystem::path const& workDirectory)
    {
        Source source{.workload = workload};
        const auto fileName = workload.isSingleFile() ? std::string_view{"/file.bin"} : std::string_view{};
        source.localPath = Paths::nativeFromUtf8(
            fmt::format("{}/source/{}{}", Paths::nativeToUtf8(workDirectory), workload.name, fileName)
        );
        source.remotePath = fmt::format("{}/source/{}{}", OpenSshServer::dataDirectory, workload.name, fileName);

        source.localFiles = createLocalSource(workload, source.localPath);
        auto remoteFiles = server.createSource(workload, source.remotePath);
        if (!remoteFiles)
            return std::unexpected{remoteFiles.error()};
        source.remoteFiles = std::move(remoteFiles).value();

        auto local = localDigest(source.localPath);
        auto remote = server.digest(source.remotePath);
        if (!local)
            return std::unexpected{local.error()};
        if (!remote)
            return std::unexpected{remote.error()};
        source.localDigest = std::move(local).value();
        source.remoteDigest = std::move(remote).value();
        return source;
    }

    /**
     * @brief The transfer of @p source in @p direction to a target that does not exist yet.
     */
    std::expected<Transfer, std::string> prepareTransfer(
        Source const& source,
        Direction direction,
        OpenSshServer const& server,
        std::filesystem::path const& workDirectory
    )
    {
        auto const& workload = source.workload;
        const auto fileName = workload.isSingleFile() ? std::string_view{"/file.bin"} : std::string_view{};
        if (direction == Direction::Download)
        {
            const auto target = localTargetRoot(workDirectory) / workload.name;
            std::filesystem::remove_all(target);
            std::filesystem::create_directories(workload.isSingleFile() ? target : target.parent_path());
            return Transfer{
                .direction = direction,
                .workload = &source.workload,
                .localPath = Paths::nativeFromUtf8(fmt::format("{}{}", Paths::nativeToUtf8(target), fileName)),
                .remotePath = source.remotePath,
                .files = source.remoteFiles,
            };
        }

        const auto target = remoteJoin(remoteTargetRoot(), workload.name);
        const auto prepared = server.execute(fmt::format(
            "rm -rf {0} && mkdir -p {1}",
            Docker::quote(target),
            Docker::quote(workload.isSingleFile() ? target : remoteTargetRoot())
        ));
        if (!prepared)
            return std::unexpected{prepared.error()};
        return Transfer{
            .direction = direction,
            .workload = &source.workload,
            .localPath = source.localPath,
            .remotePath = fmt::format("{}{}", target, fileName),
            .files = source.localFiles,
        };
    }

    std::expected<void, std::string>
    verify(Transfer const& transfer, Source const& source, OpenSshServer const& server)
    {
        const auto digest = transfer.direction == Direction::Download ? localDigest(transfer.localPath)
                                                                      : server.digest(transfer.remotePath);
        const auto& expected = transfer.direction == Direction::Download ? source.remoteDigest : source.localDigest;
        if (!digest)
            return std::unexpected{digest.error()};
        if (*digest != expected)
            return std::unexpected<std::string>{"the target differs from the source"};
        return {};
    }

    Measurement measure(
        Client& client,
        Source const& source,
        Direction direction,
        OpenSshServer const& server,
        std::filesystem::path const& workDirectory,
        int repeats
    )
    {
        Measurement measurement{.workload = &source.workload, .direction = direction, .client = client.name()};
        for (int run = 0; run < repeats; ++run)
        {
            const auto transfer = prepareTransfer(source, direction, server, workDirectory);
            if (!transfer)
            {
                measurement.failure = transfer.error();
                break;
            }

            const auto start = std::chrono::steady_clock::now();
            const auto transferred = client.transfer(*transfer);
            const auto took = std::chrono::steady_clock::now() - start - client.fixedOverhead();
            if (!transferred)
            {
                measurement.failure = transferred.error();
                break;
            }
            if (const auto verified = verify(*transfer, source, server); !verified)
            {
                measurement.failure = verified.error();
                break;
            }
            measurement.durations.push_back(std::max(std::chrono::nanoseconds{1}, took));
            fmt::print(
                "  {} {} {}: {:.3f} s\n",
                source.workload.name,
                toString(direction),
                client.name(),
                std::chrono::duration<double>{took}.count()
            );
            std::fflush(stdout);
        }
        if (!measurement.failure.empty())
            fmt::print("  {} {} {}: failed: {}\n", source.workload.name, toString(direction), client.name(), measurement.failure);
        return measurement;
    }

    int run(Arguments const& arguments, std::filesystem::path const& workDirectory)
    {
        const std::vector<Workload> workloads{
            {.name = "large", .fileCount = 1, .fileSize = arguments.largeMebibytes * mebibyte},
            {.name = "medium",
             .fileCount = arguments.mediumCount,
             .fileSize = arguments.mediumKibibytes * kibibyte,
             .filesPerDirectory = 16},
            {.name = "small",
             .fileCount = arguments.smallCount,
             .fileSize = arguments.smallKibibytes * kibibyte,
             .filesPerDirectory = 100},
        };
        std::uint64_t tmpfsBytes = 64u * mebibyte;
        for (auto const& workload : workloads)
            tmpfsBytes += 2u * workload.totalBytes() + workload.fileCount * 4u * kibibyte;

        fmt::print("Starting OpenSSH in docker...\n");
        auto server = OpenSshServer::start({
            .dockerDirectory = NUI_SFTP_BENCHMARK_DOCKER_DIRECTORY,
            .keyDirectory = workDirectory / "keys",
            .tmpfsBytes = tmpfsBytes,
        });
        if (!server)
        {
            fmt::print(stderr, "Cannot start the server: {}\n", server.error());
            return 1;
        }
        fmt::print("{} at {}\n", server->version(), server->host());

        fmt::print("Creating the files...\n");
        std::vector<Source> sources;
        for (auto const& workload : workloads)
        {
            auto source = prepareSource(workload, *server, workDirectory);
            if (!source)
            {
                fmt::print(stderr, "Cannot create the {} files: {}\n", workload.name, source.error());
                return 1;
            }
            sources.push_back(std::move(source).value());
        }

        ReportContext context{
            .date = fmt::format("{:%Y-%m-%d %H:%M}", std::chrono::system_clock::now()),
            .serverVersion = server->version(),
            .libsshVersion = ssh_version(0),
            .serverAddress = server->host(),
            .processors = std::thread::hardware_concurrency(),
            .repeats = arguments.repeats,
            .baselineClient = "OpenSSH sftp",
        };

        std::vector<Connection> connections;
        std::vector<std::unique_ptr<Client>> clients;
        auto openSsh = OpenSshClient::make({
            .host = server->host(),
            .user = std::string{OpenSshServer::user},
            .key = server->clientKey(),
            .workDirectory = workDirectory,
        });
        if (openSsh)
            clients.push_back(std::move(openSsh).value());
        else
            context.notes.push_back(fmt::format("The OpenSSH sftp baseline did not run: {}", openSsh.error()));

        // Each client gets a connection of its own, so what one learned of the link does not carry over.
        connections.reserve(3);
        for (int index = 0; index < 3; ++index)
        {
            auto connection = connect(server->host(), OpenSshServer::port);
            if (!connection)
            {
                fmt::print(stderr, "Cannot connect: {}\n", connection.error());
                return 1;
            }
            connections.push_back(std::move(connection).value());
        }
        clients.push_back(std::make_unique<LibsshClient>(connections[0]));
        clients.push_back(std::make_unique<ApplicationClient>(connections[1], false));
        clients.push_back(std::make_unique<ApplicationClient>(connections[2], true));

        std::vector<Measurement> measurements;
        for (auto const& source : sources)
        {
            for (const auto direction : {Direction::Download, Direction::Upload})
            {
                for (auto const& client : clients)
                {
                    if (client->supports(source.workload))
                        measurements.push_back(
                            measure(*client, source, direction, *server, workDirectory, arguments.repeats)
                        );
                }
            }
            std::filesystem::remove_all(localTargetRoot(workDirectory) / source.workload.name);
            if (const auto removed = server->execute(fmt::format("rm -rf {}", remoteTargetRoot())); !removed)
                fmt::print(stderr, "{}\n", removed.error());
        }

        const auto report = renderReport(context, measurements);
        std::filesystem::create_directories(arguments.output.parent_path());
        std::ofstream{arguments.output, std::ios::trunc} << report;
        fmt::print("\n{}\nWritten to {}\n", report, Paths::nativeToUtf8(arguments.output));
        return 0;
    }
}

int main(int argc, char** argv)
{
    Log::setLevel(Log::Level::Off);

    const auto programDirectory = std::filesystem::weakly_canonical(std::filesystem::path{argv[0]}.parent_path());
    const auto arguments = parseArguments(argc, argv, programDirectory);
    if (!arguments)
    {
        fmt::print(stderr, "{}", arguments.error());
        return 2;
    }

    const Utility::ProcessTemporaryDirectory processTemporary{programDirectory / "temp"};
    return run(*arguments, processTemporary.path());
}
