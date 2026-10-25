#include "connection.hpp"
#include "openssh_server.hpp"

#include <persistence/state/session_options.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Benchmark
{
    std::expected<Connection, std::string> connect(std::string const& host, unsigned short port)
    {
        using namespace std::chrono_literals;

        const Persistence::SshSessionOptions options{
            .host = host,
            .port = port,
            .users = {Persistence::SshUser{.name = std::string{OpenSshServer::user}}},
            .sshOptions =
                Persistence::SshOptions{
                    .tryAgentForAuthentication = false,
                    .usePublicKeyAutoAuth = false,
                    .usePasswordAuth = true,
                    .bypassConfig = true,
                    .connectTimeoutSeconds = 10,
                },
        };
        auto session = SecureShell::makeSession(
            options,
            +[](char const*, char* buffer, std::size_t length, int, int, void*)
            {
                const auto password = OpenSshServer::password;
                const auto copied = std::min(password.size(), length - 1);
                std::memcpy(buffer, password.data(), copied);
                buffer[copied] = '\0';
                return 0;
            },
            nullptr,
            nullptr,
            nullptr,
            []() {}
        );
        if (!session)
            return std::unexpected{fmt::format("connecting to {} failed: {}", host, session.error())};

        (*session)->start();
        auto sftpFuture = (*session)->createSftpSession();
        if (sftpFuture.wait_for(10s) != std::future_status::ready)
            return std::unexpected<std::string>{"opening the sftp channel timed out"};
        auto sftp = sftpFuture.get();
        if (!sftp)
            return std::unexpected{fmt::format("opening the sftp channel failed: {}", sftp.error().message)};

        return Connection{.session = std::move(session).value(), .sftp = sftp->lock()};
    }
}
