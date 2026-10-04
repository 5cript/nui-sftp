#pragma once

#ifndef _WIN32

#    include <backend/process/boost_process.hpp>

#    include <fmt/format.h>

#    include <sys/wait.h>
#    include <unistd.h>

#    include <array>
#    include <atomic>
#    include <cstdio>
#    include <optional>
#    include <string>
#    include <string_view>

namespace Test::Docker
{
    /**
     * @brief One image per bash major version still found on servers, macOS' 3.2 included.
     */
    inline constexpr std::array<std::string_view, 6> bashImages{
        "bash:3.2",
        "bash:4.4",
        "bash:5.0",
        "bash:5.1",
        "bash:5.2",
        "bash:5.3",
    };

    /**
     * @brief Built from docker/shells.Dockerfile, holds zsh and fish.
     */
    inline constexpr std::string_view shellsImage = "nui-sftp-test-shells";

    struct CommandResult
    {
        int exitCode{-1};
        std::string output{};
    };

    inline CommandResult run(std::string const& command)
    {
        CommandResult result;
        auto* const pipe = popen((command + " 2>&1").c_str(), "r");
        if (pipe == nullptr)
            return result;
        std::array<char, 4096> buffer{};
        while (const auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe))
            result.output.append(buffer.data(), bytes);
        const auto status = pclose(pipe);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return result;
    }

    /**
     * @brief Checks docker once per run and prepares every image (pulled only when missing).
     *
     * @return Why docker cannot be used, or nothing when all images are ready.
     */
    inline std::optional<std::string> const& unavailableReason()
    {
        static const std::optional<std::string> reason = []() -> std::optional<std::string> {
            if (boost::process::v2::environment::find_executable("docker").empty())
                return std::string{"docker is not installed. The shell integration tests run shells in containers."};

            if (const auto info = run("docker info --format '{{.ServerVersion}}'"); info.exitCode != 0)
            {
                return fmt::format(
                    "the docker daemon is not reachable. The shell integration tests run shells in containers; "
                    "start it (e.g. sudo systemctl start docker). docker said: {}",
                    info.output
                );
            }

            for (auto const image : bashImages)
            {
                if (run(fmt::format("docker image inspect {}", image)).exitCode == 0)
                    continue;
                if (const auto pull = run(fmt::format("docker pull -q {}", image)); pull.exitCode != 0)
                    return fmt::format("pulling {} failed: {}", image, pull.output);
            }

            const std::string directory = NUI_SFTP_TEST_DOCKER_DIRECTORY;
            if (const auto build =
                    run(fmt::format("docker build -q -t {} -f {}/shells.Dockerfile {}", shellsImage, directory, directory));
                build.exitCode != 0)
            {
                return fmt::format("building {} failed: {}", shellsImage, build.output);
            }
            return std::nullopt;
        }();
        return reason;
    }

    inline std::string uniqueContainerName()
    {
        static std::atomic<int> counter{0};
        return fmt::format("nui-sftp-shell-test-{}-{}", getpid(), ++counter);
    }

    /**
     * @brief Removes a container that outlived its test, e.g. because the client was killed.
     */
    inline void removeContainer(std::string const& name)
    {
        run(fmt::format("docker rm -f {}", name));
    }
}

#endif
