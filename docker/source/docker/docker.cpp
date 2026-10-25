#include <docker/docker.hpp>

#include <fmt/format.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>

namespace Docker
{
    CommandResult run(std::string const& command)
    {
        CommandResult result;
        {
            const auto closePipe = [&result](FILE* pipe)
            {
                const auto status = pclose(pipe);
                result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            };
            const std::unique_ptr<FILE, decltype(closePipe)> pipe{
                popen(fmt::format("{} 2>&1", command).c_str(), "r"), closePipe
            };
            if (!pipe)
                return result;
            std::array<char, 4096> buffer{};
            while (const auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe.get()))
                result.output.append(buffer.data(), bytes);
        }
        return result;
    }

    std::string quote(std::string_view argument)
    {
        std::string quoted{"'"};
        for (const auto character : argument)
        {
            if (character == '\'')
                quoted += R"('\'')";
            else
                quoted += character;
        }
        quoted += '\'';
        return quoted;
    }

    std::expected<void, std::string> checkAvailable()
    {
        if (run("command -v docker").exitCode != 0)
            return std::unexpected<std::string>{"docker is not installed"};

        if (const auto info = run("docker info --format '{{.ServerVersion}}'"); info.exitCode != 0)
        {
            return std::unexpected{fmt::format(
                "the docker daemon is not reachable, start it (e.g. sudo systemctl start docker). docker said: {}",
                info.output
            )};
        }
        return {};
    }

    std::expected<void, std::string> pullIfMissing(std::string_view image)
    {
        if (run(fmt::format("docker image inspect {}", quote(image))).exitCode == 0)
            return {};
        if (const auto pull = run(fmt::format("docker pull -q {}", quote(image))); pull.exitCode != 0)
            return std::unexpected{fmt::format("pulling {} failed: {}", image, pull.output)};
        return {};
    }

    std::expected<void, std::string>
    buildImage(std::string_view tag, std::filesystem::path const& dockerfile, std::filesystem::path const& context)
    {
        const auto build = run(fmt::format(
            "docker build -q -t {} -f {} {}", quote(tag), quote(dockerfile.string()), quote(context.string())
        ));
        if (build.exitCode != 0)
            return std::unexpected{fmt::format("building {} failed: {}", tag, build.output)};
        return {};
    }

    std::string uniqueContainerName(std::string_view prefix)
    {
        static std::atomic<int> counter{0};
        return fmt::format("{}-{}-{}", prefix, getpid(), ++counter);
    }

    void removeContainer(std::string_view name)
    {
        run(fmt::format("docker rm -f {}", quote(name)));
    }
}
