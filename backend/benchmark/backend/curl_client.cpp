#include "curl_client.hpp"
#include "operation_driver.hpp"
#include "processor_time.hpp"

#include <docker/docker.hpp>
#include <paths/native.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace Benchmark
{
    namespace
    {
        /**
         * @brief @p text as a quoted value of a curl config file.
         */
        std::string configQuoted(std::string_view text)
        {
            std::string quoted{"\""};
            for (const auto character : text)
            {
                if (character == '"' || character == '\\')
                    quoted += '\\';
                quoted += character;
            }
            quoted += '"';
            return quoted;
        }

        bool isUnreserved(char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
                character == '~' || character == '/';
        }

        /**
         * @brief An absolute path as the path of an FTP URL: the leading slash as %2F, which makes it absolute.
         */
        std::string urlPathOf(std::string_view absolutePath)
        {
            std::string encoded{"%2F"};
            for (const auto character : absolutePath.substr(absolutePath.starts_with('/') ? 1u : 0u))
            {
                if (isUnreserved(character))
                    encoded += character;
                else
                    encoded += fmt::format("%{:02X}", static_cast<unsigned char>(character));
            }
            return encoded;
        }

        std::string joined(std::string_view root, std::string_view name)
        {
            return fmt::format("{}/{}", root, name);
        }
    }

    CurlClient::CurlClient(Options options)
        : options_{std::move(options)}
    {}

    std::expected<std::unique_ptr<CurlClient>, std::string> CurlClient::make(Options options)
    {
        if (Docker::run("command -v curl").exitCode != 0)
            return std::unexpected<std::string>{"curl is not installed on the host"};

        std::unique_ptr<CurlClient> client{new CurlClient{std::move(options)}};
        std::array<std::chrono::nanoseconds, 5> overheads{};
        std::array<std::chrono::nanoseconds, 5> processorOverheads{};
        for (std::size_t index = 0; index < overheads.size(); ++index)
        {
            const auto processorStart = processorTime();
            const auto start = std::chrono::steady_clock::now();
            const auto probe = configQuoted(client->urlOf(client->options_.probePath));
            const auto ran = client->run(fmt::format("head\nurl = {}\noutput = \"/dev/null\"\n", probe));
            if (!ran)
                return std::unexpected{ran.error()};
            overheads[index] = std::chrono::steady_clock::now() - start;
            processorOverheads[index] = processorTime() - processorStart;
        }
        std::ranges::sort(overheads);
        std::ranges::sort(processorOverheads);
        client->overhead_ = overheads[overheads.size() / 2];
        client->processorOverhead_ = processorOverheads[processorOverheads.size() / 2];
        return client;
    }

    std::string CurlClient::nameFor(Protocol protocol)
    {
        return protocol == Protocol::Ftps ? "curl ftps" : "curl ftp";
    }

    std::string CurlClient::name() const
    {
        return nameFor(options_.protocol);
    }

    Protocol CurlClient::protocol() const
    {
        return options_.protocol;
    }

    std::string CurlClient::urlOf(std::string const& remotePath) const
    {
        return fmt::format("ftp://{}:{}/{}", options_.host, options_.port, urlPathOf(remotePath));
    }

    std::expected<void, std::string> CurlClient::transfer(Transfer const& transfer)
    {
        const auto download = transfer.direction == Direction::Download;
        std::string requests = download ? "create-dirs\n" : "ftp-create-dirs\n";
        if (!transfer.workload->isSingleFile())
            requests += fmt::format("parallel\nparallel-max = {}\n", bulkConcurrency());

        const auto addRequest = [&](std::filesystem::path const& local, std::string const& remote)
        {
            const auto localText = configQuoted(Paths::nativeToUtf8(local));
            const auto url = configQuoted(urlOf(remote));
            if (download)
                requests += fmt::format("url = {}\noutput = {}\n", url, localText);
            else
                requests += fmt::format("upload-file = {}\nurl = {}\n", localText, url);
        };
        if (transfer.workload->isSingleFile())
            addRequest(transfer.localPath, transfer.remotePath);
        for (auto const& file : transfer.files)
            addRequest(transfer.localPath / Paths::nativeFromUtf8(file), joined(transfer.remotePath, file));
        return run(requests);
    }

    std::expected<void, std::string> CurlClient::run(std::string const& requests) const
    {
        const auto configFile = options_.workDirectory / "curl-config";
        {
            std::ofstream config{configFile, std::ios::trunc};
            config << "silent\nshow-error\nfail-early\n";
            config << fmt::format("user = {}\n", configQuoted(fmt::format("{}:{}", options_.user, options_.password)));
            if (options_.protocol == Protocol::Ftps)
                config << "ssl-reqd\ninsecure\n";
            config << requests;
        }
        const auto result = Docker::run(fmt::format("curl -q -K {}", Docker::quote(Paths::nativeToUtf8(configFile))));
        if (result.exitCode != 0)
            return std::unexpected{fmt::format("curl failed: {}", result.output)};
        return {};
    }
}
