#include <ftp/client/socket_options.hpp>
#include <log/log.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/socket_base.hpp>
#include <fmt/format.h>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#endif

#include <cstddef>
#include <string_view>

namespace FileTransferProtocol
{
    namespace
    {
        using boost::asio::ip::tcp;
        using Result = std::expected<void, RemoteIo::RemoteError>;

        /**
         * @brief An int socket option Asio has no type for.
         */
        class IntegerOption
        {
          public:
            IntegerOption(int level, int option, int value)
                : level_{level}
                , option_{option}
                , value_{value}
            {}

            template <typename ProtocolT>
            int level(ProtocolT const&) const
            {
                return level_;
            }
            template <typename ProtocolT>
            int name(ProtocolT const&) const
            {
                return option_;
            }
            template <typename ProtocolT>
            int const* data(ProtocolT const&) const
            {
                return &value_;
            }
            template <typename ProtocolT>
            std::size_t size(ProtocolT const&) const
            {
                return sizeof(value_);
            }

          private:
            int level_;
            int option_;
            int value_;
        };

        Result refused(std::string_view optionName, boost::system::error_code error)
        {
            return std::unexpected(
                RemoteIo::RemoteError{
                    .kind = RemoteIo::RemoteErrorKind::Failure,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .message =
                        fmt::format("The system refused {} on the control socket: {}", optionName, error.message()),
                }
            );
        }

        template <typename OptionT>
        Result setOption(tcp::socket& socket, OptionT const& option, std::string_view optionName)
        {
            boost::system::error_code error{};
            socket.set_option(option, error);
            if (error)
                return refused(optionName, error);
            return {};
        }

        Result setKeepaliveIdle([[maybe_unused]] tcp::socket& socket, [[maybe_unused]] std::chrono::seconds idle)
        {
#if defined(TCP_KEEPIDLE)
            return Detail::setTuningOption(
                socket, IPPROTO_TCP, TCP_KEEPIDLE, static_cast<int>(idle.count()), "TCP_KEEPIDLE"
            );
#elif defined(TCP_KEEPALIVE)
            // The name of the idle time on macOS.
            return Detail::setTuningOption(
                socket, IPPROTO_TCP, TCP_KEEPALIVE, static_cast<int>(idle.count()), "TCP_KEEPALIVE"
            );
#else
            return {};
#endif
        }

        Result
        setKeepaliveInterval([[maybe_unused]] tcp::socket& socket, [[maybe_unused]] std::chrono::seconds interval)
        {
#if defined(TCP_KEEPINTVL)
            return Detail::setTuningOption(
                socket, IPPROTO_TCP, TCP_KEEPINTVL, static_cast<int>(interval.count()), "TCP_KEEPINTVL"
            );
#else
            return {};
#endif
        }

        Result setKeepaliveCount([[maybe_unused]] tcp::socket& socket, [[maybe_unused]] int count)
        {
#if defined(TCP_KEEPCNT)
            return Detail::setTuningOption(socket, IPPROTO_TCP, TCP_KEEPCNT, count, "TCP_KEEPCNT");
#else
            return {};
#endif
        }

        Result setUserTimeout([[maybe_unused]] tcp::socket& socket, [[maybe_unused]] std::chrono::milliseconds timeout)
        {
#if defined(TCP_USER_TIMEOUT)
            return Detail::setTuningOption(
                socket, IPPROTO_TCP, TCP_USER_TIMEOUT, static_cast<int>(timeout.count()), "TCP_USER_TIMEOUT"
            );
#else
            return {};
#endif
        }
    }

    namespace Detail
    {
        Result setTuningOption(tcp::socket& socket, int level, int option, int value, std::string_view optionName)
        {
            boost::system::error_code error{};
            socket.set_option(IntegerOption{level, option, value}, error);
            // The headers can name an option the running system lacks, an older Windows for one.
            if (error == boost::asio::error::no_protocol_option)
            {
                Log::debug("FTP: The system has no {}, it keeps its own value", optionName);
                return {};
            }
            if (error)
                return refused(optionName, error);
            return {};
        }
    }

    std::chrono::milliseconds effectiveUserTimeout(ControlSocketOptions const& options)
    {
        auto const& keepalive = options.keepalive;
        return options.userTimeout.value_or(keepalive.idle + keepalive.interval * keepalive.count);
    }

    Result applyControlSocketOptions(tcp::socket& socket, ControlSocketOptions const& options)
    {
        auto const& keepalive = options.keepalive;
        return setOption(socket, tcp::no_delay{true}, "TCP_NODELAY")
            .and_then(
                [&]()
                {
                    return setOption(socket, boost::asio::socket_base::keep_alive{keepalive.enabled}, "SO_KEEPALIVE");
                }
            )
            .and_then(
                [&]()
                {
                    return keepalive.enabled ? setKeepaliveIdle(socket, keepalive.idle) : Result{};
                }
            )
            .and_then(
                [&]()
                {
                    return keepalive.enabled ? setKeepaliveInterval(socket, keepalive.interval) : Result{};
                }
            )
            .and_then(
                [&]()
                {
                    return keepalive.enabled ? setKeepaliveCount(socket, keepalive.count) : Result{};
                }
            )
            .and_then(
                [&]()
                {
                    return setUserTimeout(socket, effectiveUserTimeout(options));
                }
            );
    }
}
