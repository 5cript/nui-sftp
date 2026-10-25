#pragma once

#include "workload.hpp"

#include <docker/container.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Benchmark
{
    /**
     * @brief OpenSSH with its sftp-server in a container, the transferred files on a tmpfs so the disk of the host is
     *        out of the measurement on that side.
     */
    class OpenSshServer
    {
      public:
        static constexpr unsigned short port = 22;
        static constexpr std::string_view user = "bench";
        static constexpr std::string_view password = "bench";
        /** @brief The tmpfs the transfers read from and write to. */
        static constexpr std::string_view dataDirectory = "/home/bench/data";

        struct Options
        {
            /** @brief Holds openssh.Dockerfile and sshd_config. */
            std::filesystem::path dockerDirectory{};
            /** @brief Where the key pair of the OpenSSH client baseline is created. */
            std::filesystem::path keyDirectory{};
            std::uint64_t tmpfsBytes{0};
        };

        /**
         * @brief Builds the image (cached by docker), starts the container and waits until sshd answers.
         */
        static std::expected<OpenSshServer, std::string> start(Options const& options);

        ~OpenSshServer() = default;
        OpenSshServer(OpenSshServer const&) = delete;
        OpenSshServer& operator=(OpenSshServer const&) = delete;
        OpenSshServer(OpenSshServer&&) = default;
        OpenSshServer& operator=(OpenSshServer&&) = default;

        /** @brief The address of the container, reached without a published port and its userland proxy. */
        std::string const& host() const noexcept
        {
            return host_;
        }

        /** @brief The private key the server accepts for @ref user, empty when ssh-keygen is missing. */
        std::filesystem::path const& clientKey() const noexcept
        {
            return clientKey_;
        }

        /** @brief What sshd -V says, e.g. OpenSSH_10.3p1. */
        std::string const& version() const noexcept
        {
            return version_;
        }

        /**
         * @brief Runs @p command in the container as @ref user.
         *
         * @return Its output, or why it failed.
         */
        std::expected<std::string, std::string> execute(std::string_view command) const;

        /**
         * @brief Creates the files of @p workload below @p remotePath with random content.
         *
         * @return The created files relative to @p remotePath, empty for a single file.
         */
        std::expected<std::vector<std::string>, std::string>
        createSource(Workload const& workload, std::string_view remotePath) const;

        /**
         * @brief A digest of the file or directory tree @p remotePath: names and contents.
         */
        std::expected<std::string, std::string> digest(std::string_view remotePath) const;

      private:
        OpenSshServer(Docker::Container container, std::string host);

      private:
        Docker::Container container_;
        std::string host_;
        std::filesystem::path clientKey_{};
        std::string version_{};
    };

    /**
     * @brief The shell command that prints the digest of the file or directory tree @p path, the same on the host and
     *        in the container.
     */
    std::string digestCommand(std::string_view path);

    /**
     * @brief The digest of the local file or directory tree @p path, see digestCommand().
     */
    std::expected<std::string, std::string> localDigest(std::filesystem::path const& path);
}
