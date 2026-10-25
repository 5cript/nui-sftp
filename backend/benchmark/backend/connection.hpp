#pragma once

#include <ssh/session.hpp>
#include <ssh/sftp_session.hpp>

#include <expected>
#include <memory>
#include <string>

namespace Benchmark
{
    /**
     * @brief An SSH session with one SFTP channel, opened like the app does with password authentication.
     */
    struct Connection
    {
        std::unique_ptr<SecureShell::Session> session{};
        std::shared_ptr<SecureShell::SftpSession> sftp{};
    };

    std::expected<Connection, std::string> connect(std::string const& host, unsigned short port);
}
