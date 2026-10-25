#pragma once

#ifndef _WIN32

#    include <docker/docker.hpp>

#    include <fmt/format.h>

#    include <array>
#    include <optional>
#    include <string>
#    include <string_view>

namespace Test::DockerShells
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

    /**
     * @brief Checks docker once per run and prepares every image (pulled only when missing).
     *
     * @return Why docker cannot be used, or nothing when all images are ready.
     */
    inline std::optional<std::string> const& unavailableReason()
    {
        static const std::optional<std::string> reason = []() -> std::optional<std::string> {
            if (const auto available = Docker::checkAvailable(); !available)
                return fmt::format("{}. The shell integration tests run shells in containers.", available.error());

            for (auto const image : bashImages)
            {
                if (const auto pulled = Docker::pullIfMissing(image); !pulled)
                    return pulled.error();
            }

            const std::filesystem::path directory = NUI_SFTP_TEST_DOCKER_DIRECTORY;
            if (const auto built = Docker::buildImage(shellsImage, directory / "shells.Dockerfile", directory); !built)
                return built.error();
            return std::nullopt;
        }();
        return reason;
    }

    inline std::string uniqueContainerName()
    {
        return Docker::uniqueContainerName("nui-sftp-shell-test");
    }
}

#endif
