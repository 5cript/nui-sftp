#pragma once

#include <ftp/protocol/command.hpp>
#include <ftp/protocol/reply.hpp>
#include <remote_io/remote_error.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief A time-val of RFC 3659 2.3, always UTC. Fractions finer than a microsecond are cut. Years past 2262
     *        overflow a system_clock::time_point of libstdc++, which counts nanoseconds: convert with floor<seconds>.
     */
    using TimeVal = std::chrono::sys_time<std::chrono::microseconds>;

    /**
     * @brief Reads a time-val, YYYYMMDDHHMMSS with optional fractions after a dot. Second 60 is a leap second.
     */
    std::expected<TimeVal, RemoteIo::RemoteError> parseTimeVal(std::string_view text);

    /**
     * @brief The time-val of @p time, as MFMT and MDTM with a time take it.
     *
     * @return Nothing for a year outside 1000 to 9999, which a time-val cannot hold.
     */
    std::optional<std::string> formatTimeVal(std::chrono::sys_seconds time);

    /**
     * @brief The size from the 213 reply to SIZE (RFC 3659 4), up to 2^64 - 1.
     */
    std::expected<std::uint64_t, RemoteIo::RemoteError> parseSizeReply(Reply const& reply);

    /**
     * @brief The time from the 213 reply to MDTM (RFC 3659 3).
     */
    std::expected<TimeVal, RemoteIo::RemoteError> parseModificationTimeReply(Reply const& reply);

    /**
     * @brief Where a 227 reply says the server listens. The client connects to the address of the control
     *        connection unless the session trusts the announced one (RFC 2577).
     */
    struct PassiveEndpoint
    {
        std::array<std::uint8_t, 4> address{};
        std::uint16_t port{0};
    };

    /**
     * @brief Reads the 227 reply to PASV by scanning it for the six numbers h1,h2,h3,h4,p1,p2, with or without the
     *        parentheses (RFC 1123 4.1.2.6).
     */
    std::expected<PassiveEndpoint, RemoteIo::RemoteError> parsePassiveReply(Reply const& reply);

    /**
     * @brief The port of the 229 reply to EPSV, "(|||port|)" with any delimiter from ASCII 33 to 126 other than a
     *        digit, the same one each time (RFC 2428 3).
     */
    std::expected<std::uint16_t, RemoteIo::RemoteError> parseExtendedPassiveReply(Reply const& reply);

    /**
     * @brief The quoted path of a 257 reply to PWD or MKD, doubled quotes undoubled (RFC 959 Appendix II) and CR NUL
     *        read as CR (RFC 2640 3.1).
     *
     * @param verb The command the reply answers, for the error of any other reply.
     */
    std::expected<std::string, RemoteIo::RemoteError> parsePathReply(Reply const& reply, Verb verb);
}
