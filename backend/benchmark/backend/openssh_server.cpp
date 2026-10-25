#include "openssh_server.hpp"

#include <docker/docker.hpp>

#include <fmt/format.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace Benchmark
{
    namespace
    {
        constexpr std::string_view image = "nui-sftp-benchmark-openssh";

        std::string trimmed(std::string text)
        {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            return text;
        }

        class UniqueSocket
        {
          public:
            UniqueSocket()
                : descriptor_{::socket(AF_INET, SOCK_STREAM, 0)}
            {}
            ~UniqueSocket()
            {
                if (descriptor_ >= 0)
                    ::close(descriptor_);
            }
            UniqueSocket(UniqueSocket const&) = delete;
            UniqueSocket& operator=(UniqueSocket const&) = delete;
            UniqueSocket(UniqueSocket&&) = delete;
            UniqueSocket& operator=(UniqueSocket&&) = delete;

            int get() const noexcept
            {
                return descriptor_;
            }

          private:
            int descriptor_;
        };

        /**
         * @brief Whether a TCP connection to @p host and @p port gets an SSH banner.
         */
        bool answersWithBanner(std::string const& host, unsigned short port)
        {
            const UniqueSocket socket;
            if (socket.get() < 0)
                return false;

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
                return false;
            if (::connect(socket.get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
                return false;

            std::array<char, 4> banner{};
            return ::recv(socket.get(), banner.data(), banner.size(), MSG_WAITALL) ==
                static_cast<ssize_t>(banner.size()) &&
                std::string_view{banner.data(), banner.size()} == "SSH-";
        }

        std::expected<void, std::string> waitUntilAnswering(std::string const& host)
        {
            using namespace std::chrono_literals;
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (answersWithBanner(host, OpenSshServer::port))
                    return {};
                std::this_thread::sleep_for(100ms);
            }
            return std::unexpected{fmt::format("sshd at {} did not answer within 20 s", host)};
        }

        /**
         * @brief Creates a key pair in @p directory with the host's ssh-keygen.
         *
         * @return The private key, empty when ssh-keygen is missing.
         */
        std::filesystem::path createClientKey(std::filesystem::path const& directory)
        {
            const auto key = directory / "client_key";
            std::filesystem::create_directories(directory);
            std::filesystem::remove(key);
            std::filesystem::remove(key.string() + ".pub");
            const auto generated =
                Docker::run(fmt::format("ssh-keygen -q -t ed25519 -N '' -f {}", Docker::quote(key.string())));
            if (generated.exitCode != 0)
                return {};
            return key;
        }
    }

    std::string digestCommand(std::string_view path)
    {
        return fmt::format(
            "cd {} 2>/dev/null && {{ find . -type f | LC_ALL=C sort; find . -type f | LC_ALL=C sort | xargs cat; }} | "
            "sha256sum || sha256sum < {}",
            Docker::quote(path),
            Docker::quote(path)
        );
    }

    std::expected<std::string, std::string> localDigest(std::filesystem::path const& path)
    {
        const auto result = Docker::run(digestCommand(path.string()));
        if (result.exitCode != 0)
            return std::unexpected{fmt::format("digest of {} failed: {}", path.string(), result.output)};
        return trimmed(result.output).substr(0, 64);
    }

    OpenSshServer::OpenSshServer(Docker::Container container, std::string host)
        : container_{std::move(container)}
        , host_{std::move(host)}
    {}

    std::expected<OpenSshServer, std::string> OpenSshServer::start(Options const& options)
    {
        return Docker::checkAvailable()
            .and_then(
                [&options]()
                {
                    return Docker::buildImage(
                        image, options.dockerDirectory / "openssh.Dockerfile", options.dockerDirectory
                    );
                }
            )
            .and_then(
                [&options]()
                {
                    return Docker::Container::start({
                        .image = std::string{image},
                        .namePrefix = "nui-sftp-benchmark",
                        .runArguments =
                            {"--tmpfs",
                             fmt::format(
                                 "{}:rw,size={},mode=0755,uid=1000,gid=1000", dataDirectory, options.tmpfsBytes
                             )},
                    });
                }
            )
            .and_then(
                [](Docker::Container container)
                {
                    return container.ipAddress().transform(
                        [&container](std::string host)
                        {
                            return OpenSshServer{std::move(container), std::move(host)};
                        }
                    );
                }
            )
            .and_then(
                [&options](OpenSshServer server) -> std::expected<OpenSshServer, std::string>
                {
                    if (const auto answering = waitUntilAnswering(server.host_); !answering)
                        return std::unexpected{answering.error()};

                    server.version_ = trimmed(server.container_.execute("sshd -V 2>&1 | head -n 1").output);

                    if (const auto key = createClientKey(options.keyDirectory); !key.empty())
                    {
                        std::ifstream publicKeyFile{key.string() + ".pub"};
                        std::stringstream publicKey;
                        publicKey << publicKeyFile.rdbuf();
                        const auto installed = server.execute(fmt::format(
                            "printf '%s\\n' {} > ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys",
                            Docker::quote(trimmed(publicKey.str()))
                        ));
                        if (!installed)
                            return std::unexpected{installed.error()};
                        server.clientKey_ = key;
                    }
                    return server;
                }
            );
    }

    std::expected<std::string, std::string> OpenSshServer::execute(std::string_view command) const
    {
        const auto result = container_.execute(command, user);
        if (result.exitCode != 0)
            return std::unexpected{fmt::format("'{}' failed in the container: {}", command, result.output)};
        return result.output;
    }

    std::expected<std::vector<std::string>, std::string>
    OpenSshServer::createSource(Workload const& workload, std::string_view remotePath) const
    {
        if (workload.isSingleFile())
        {
            return execute(fmt::format(
                           "mkdir -p \"$(dirname {0})\" && head -c {1} /dev/urandom > {0}",
                           Docker::quote(remotePath),
                           workload.fileSize
                       ))
                .transform(
                    [](auto const&)
                    {
                        return std::vector<std::string>{};
                    }
                );
        }

        std::string script = fmt::format("mkdir -p {0} && cd {0}", Docker::quote(remotePath));
        for (std::uint64_t first = 0, directory = 0; first < workload.fileCount;
             first += workload.filesPerDirectory, ++directory)
        {
            const auto count = std::min(workload.filesPerDirectory, workload.fileCount - first);
            script += fmt::format(
                " && mkdir d{0:03} && head -c {1} /dev/urandom | (cd d{0:03} && split -b {2} -a 5 - f)",
                directory,
                count * workload.fileSize,
                workload.fileSize
            );
        }
        script += " && find . -type f | LC_ALL=C sort";

        return execute(script).transform(
            [](std::string const& listing)
            {
                std::vector<std::string> files;
                std::istringstream lines{listing};
                for (std::string line; std::getline(lines, line);)
                {
                    if (line.starts_with("./"))
                        files.push_back(line.substr(2));
                }
                return files;
            }
        );
    }

    std::expected<std::string, std::string> OpenSshServer::digest(std::string_view remotePath) const
    {
        return execute(digestCommand(remotePath)).transform(
            [](std::string const& output)
            {
                return trimmed(output).substr(0, 64);
            }
        );
    }
}
