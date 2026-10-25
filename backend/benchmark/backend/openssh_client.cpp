#include "openssh_client.hpp"

#include <docker/docker.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace Benchmark
{
    namespace
    {
        /**
         * @brief A batch path for sftp, which splits its arguments at blanks unless they are quoted.
         */
        std::string batchQuoted(std::string_view path)
        {
            std::string quoted{"\""};
            for (const auto character : path)
            {
                if (character == '"' || character == '\\')
                    quoted += '\\';
                quoted += character;
            }
            quoted += '"';
            return quoted;
        }
    }

    OpenSshClient::OpenSshClient(Options options)
        : options_{std::move(options)}
    {}

    std::expected<std::unique_ptr<OpenSshClient>, std::string> OpenSshClient::make(Options options)
    {
        if (Docker::run("command -v sftp").exitCode != 0)
            return std::unexpected<std::string>{"sftp is not installed on the host"};
        if (options.key.empty())
            return std::unexpected<std::string>{"ssh-keygen is not installed on the host, the server has no key"};

        std::unique_ptr<OpenSshClient> client{new OpenSshClient{std::move(options)}};
        std::array<std::chrono::nanoseconds, 5> overheads{};
        for (auto& overhead : overheads)
        {
            const auto start = std::chrono::steady_clock::now();
            if (const auto ran = client->runBatch("pwd\n"); !ran)
                return std::unexpected{ran.error()};
            overhead = std::chrono::steady_clock::now() - start;
        }
        std::ranges::sort(overheads);
        client->overhead_ = overheads[overheads.size() / 2];
        return client;
    }

    std::string OpenSshClient::name() const
    {
        return "OpenSSH sftp";
    }

    std::expected<void, std::string> OpenSshClient::transfer(Transfer const& transfer)
    {
        const auto recursive = transfer.workload->isSingleFile() ? "" : "-r ";
        const auto local = batchQuoted(transfer.localPath.string());
        const auto remote = batchQuoted(transfer.remotePath);
        if (transfer.direction == Direction::Download)
            return runBatch(fmt::format("get {}{} {}\n", recursive, remote, local));
        return runBatch(fmt::format("put {}{} {}\n", recursive, local, remote));
    }

    std::expected<void, std::string> OpenSshClient::runBatch(std::string const& commands) const
    {
        const auto batchFile = options_.workDirectory / "sftp-batch";
        {
            std::ofstream batch{batchFile, std::ios::trunc};
            batch << commands;
        }
        const auto result = Docker::run(fmt::format(
            "sftp -q -F /dev/null -i {} -o IdentitiesOnly=yes -o IdentityAgent=none -o BatchMode=yes "
            "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -b {} {}@{}",
            Docker::quote(options_.key.string()),
            Docker::quote(batchFile.string()),
            options_.user,
            options_.host
        ));
        if (result.exitCode != 0)
            return std::unexpected{fmt::format("sftp failed: {}", result.output)};
        return {};
    }
}
