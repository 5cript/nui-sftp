#ifndef _WIN32

#    include <ftp/support/dropped_connection.hpp>

#    include <docker/docker.hpp>

#    include <fmt/format.h>

#    include <signal.h>
#    include <unistd.h>

#    include <cerrno>
#    include <charconv>
#    include <cstdio>
#    include <optional>
#    include <ranges>
#    include <string_view>
#    include <system_error>
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

        /** @brief Starts the comment of every rule, the process that added the rule follows. */
        constexpr std::string_view ownerTag = "nui-sftp-drop-";

        /**
         * @brief The process that added @p rule, a line of iptables -S. Nothing for a rule of someone else.
         */
        std::optional<pid_t> ownerOf(std::string_view rule)
        {
            const auto tag = rule.find(ownerTag);
            if (tag == std::string_view::npos)
                return std::nullopt;
            const auto digits = rule.substr(tag + ownerTag.size());
            pid_t owner{};
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), owner);
            if (parsed.ec != std::errc{})
                return std::nullopt;
            return owner;
        }

        bool hasEnded(pid_t process)
        {
            return ::kill(process, 0) == -1 && errno == ESRCH;
        }

        /**
         * @brief Deletes the rules of test processes that ended without their destructors, killed at a timeout say.
         *        Such a rule would drop the connection of a later test that comes from the same port.
         */
        void deleteRulesOfEndedProcesses(std::string const& container)
        {
            const auto listed = Docker::executeIn(container, "iptables -w -S");
            if (listed.exitCode != 0)
                return;
            const std::string_view append = "-A ";
            for (const auto line : std::views::split(std::string_view{listed.output}, '\n'))
            {
                const std::string_view rule{line.begin(), line.end()};
                const auto owner = ownerOf(rule);
                // Another process may delete the same rule at the same time, one of the two then finds it gone.
                if (owner && rule.starts_with(append) && hasEnded(*owner))
                    iptables(container, "-D", rule.substr(append.size()));
            }
        }

        void deleteRules(std::string const& container, std::vector<std::string> const& rules)
        {
            for (auto const& rule : rules)
            {
                if (const auto result = iptables(container, "-D", rule); result.exitCode != 0)
                    fmt::print(
                        stderr,
                        "iptables -D {} failed in {}, the rule keeps dropping packets: {}\n",
                        rule,
                        container,
                        result.output
                    );
            }
        }
    }

    std::expected<DroppedConnection, std::string>
    DroppedConnection::drop(std::string container, std::uint16_t serverPort, std::uint16_t clientPort)
    {
        deleteRulesOfEndedProcesses(container);
        const auto owner = fmt::format("-m comment --comment {}{}", ownerTag, ::getpid());
        const std::vector<std::string> rules{
            fmt::format("INPUT -p tcp --sport {} --dport {} {} -j DROP", clientPort, serverPort, owner),
            fmt::format("OUTPUT -p tcp --sport {} --dport {} {} -j DROP", serverPort, clientPort, owner),
        };
        std::vector<std::string> added{};
        for (auto const& rule : rules)
        {
            const auto result = iptables(container, "-I", rule);
            if (result.exitCode != 0)
            {
                // Nothing was dropped yet, the server's end of the connection stays.
                deleteRules(container, added);
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
        deleteRules(container_, std::exchange(rules_, {}));
    }
}

#endif
