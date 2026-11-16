#ifndef _WIN32

#    include <ftp/support/dropped_connection.hpp>

#    include <docker/docker.hpp>

#    include <fmt/format.h>

#    include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        /**
         * @brief -w waits for the lock, tests running in parallel change the rules of one container.
         */
        Docker::CommandResult iptables(std::string const& container, std::string_view action, std::string_view rule)
        {
            return Docker::executeIn(container, fmt::format("iptables -w {} {}", action, rule));
        }
    }

    std::expected<DroppedConnection, std::string>
    DroppedConnection::drop(std::string container, std::uint16_t serverPort, std::uint16_t clientPort)
    {
        const std::vector<std::string> rules{
            fmt::format("INPUT -p tcp --sport {} --dport {} -j DROP", clientPort, serverPort),
            fmt::format("OUTPUT -p tcp --sport {} --dport {} -j DROP", serverPort, clientPort),
        };
        std::vector<std::string> added{};
        for (auto const& rule : rules)
        {
            const auto result = iptables(container, "-I", rule);
            if (result.exitCode != 0)
            {
                DroppedConnection partial{std::move(container), serverPort, clientPort, std::move(added)};
                return std::unexpected(fmt::format("iptables -I {}: {}", rule, result.output));
            }
            added.push_back(rule);
        }
        return DroppedConnection{std::move(container), serverPort, clientPort, std::move(added)};
    }

    DroppedConnection::DroppedConnection(
        std::string container,
        std::uint16_t serverPort,
        std::uint16_t clientPort,
        std::vector<std::string> rules
    )
        : container_{std::move(container)}
        , serverPort_{serverPort}
        , clientPort_{clientPort}
        , rules_{std::move(rules)}
    {}

    DroppedConnection::~DroppedConnection()
    {
        restore();
    }

    DroppedConnection::DroppedConnection(DroppedConnection&& other) noexcept
        : container_{std::move(other.container_)}
        , serverPort_{other.serverPort_}
        , clientPort_{other.clientPort_}
        , rules_{std::exchange(other.rules_, {})}
    {}

    DroppedConnection& DroppedConnection::operator=(DroppedConnection&& other) noexcept
    {
        if (this != &other)
        {
            restore();
            container_ = std::move(other.container_);
            serverPort_ = other.serverPort_;
            clientPort_ = other.clientPort_;
            rules_ = std::exchange(other.rules_, {});
        }
        return *this;
    }

    void DroppedConnection::restore()
    {
        if (rules_.empty())
            return;
        // The server never learns that the client gave up, its session would wait for its idle timeout.
        Docker::executeIn(
            container_, fmt::format("ss -K -tn '( sport = :{} and dport = :{} )' > /dev/null", serverPort_, clientPort_)
        );
        for (auto const& rule : rules_)
            iptables(container_, "-D", rule);
        rules_.clear();
    }
}

#endif
