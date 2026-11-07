#include <ftp/protocol/reply_parsers.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_error.hpp>
#include "text.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        bool isDigit(char character)
        {
            return character >= '0' && character <= '9';
        }

        /**
         * @brief The unsigned number of the digits at @p position, which moves behind them.
         */
        template <typename NumberT>
        std::optional<NumberT> numberAt(std::string_view text, std::size_t& position)
        {
            NumberT number{};
            const auto parsed = std::from_chars(text.data() + position, text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr == text.data() + position)
                return std::nullopt;
            position = static_cast<std::size_t>(parsed.ptr - text.data());
            return number;
        }

        unsigned digitsAt(std::string_view text, std::size_t position, std::size_t count)
        {
            unsigned value = 0u;
            for (std::size_t offset = 0u; offset < count; ++offset)
                value = value * 10u + static_cast<unsigned>(text[position + offset] - '0');
            return value;
        }

        /**
         * @brief The six numbers of a 227 reply that start at @p position: nothing when fewer follow there, an error
         *        when they are no host and port.
         */
        std::optional<std::expected<PassiveEndpoint, RemoteIo::RemoteError>>
        passiveEndpointAt(std::string_view text, std::size_t& position, int code)
        {
            std::array<unsigned, 6> numbers{};
            std::size_t count = 0u;
            // The caller starts at a digit.
            bool numberFollows = true;
            while (numberFollows && count < numbers.size())
            {
                const auto number = numberAt<unsigned>(text, position);
                if (!number)
                    return std::unexpected(protocolViolation("A number of the passive reply is too large", code));
                numbers[count++] = *number;
                numberFollows = position + 1u < text.size() && text[position] == ',' && isDigit(text[position + 1u]);
                if (numberFollows)
                    ++position;
            }
            if (count < numbers.size())
                return std::nullopt;
            if (numberFollows)
                return std::unexpected(protocolViolation("The passive reply has more than six numbers", code));

            for (const auto number : numbers)
            {
                if (number > 255u)
                    return std::unexpected(protocolViolation("A number of the passive reply is above 255", code));
            }
            const auto port = numbers[4] * 256u + numbers[5];
            if (port == 0u)
                return std::unexpected(protocolViolation("The passive reply names port 0", code));
            return PassiveEndpoint{
                .address =
                    {static_cast<std::uint8_t>(numbers[0]),
                        static_cast<std::uint8_t>(numbers[1]),
                        static_cast<std::uint8_t>(numbers[2]),
                        static_cast<std::uint8_t>(numbers[3])},
                .port = static_cast<std::uint16_t>(port),
            };
        }

        std::optional<std::expected<std::uint16_t, RemoteIo::RemoteError>>
        extendedPassivePortIn(std::string_view text, int code)
        {
            const auto open = text.find('(');
            if (open == std::string_view::npos)
                return std::nullopt;
            const auto fields = text.substr(open + 1u);
            if (fields.empty())
                return std::unexpected(
                    protocolViolation("The extended passive reply ends after its parenthesis", code)
                );

            const auto delimiter = fields.front();
            if (delimiter < 33 || delimiter > 126 || isDigit(delimiter))
                return std::unexpected(protocolViolation("The extended passive reply has no valid delimiter", code));
            if (fields.size() < 3u || fields[1] != delimiter || fields[2] != delimiter)
                return std::unexpected(
                    protocolViolation("The extended passive reply does not start with three delimiters", code)
                );

            std::size_t position = 3u;
            const auto port = numberAt<std::uint32_t>(fields, position);
            if (!port)
                return std::unexpected(protocolViolation("The extended passive reply has no port", code));
            if (position >= fields.size() || fields[position] != delimiter)
                return std::unexpected(protocolViolation("The extended passive reply does not end its port", code));
            if (*port == 0u || *port > 65535u)
                return std::unexpected(
                    protocolViolation(fmt::format("The extended passive port {} is invalid", *port), code)
                );
            return static_cast<std::uint16_t>(*port);
        }
    }

    std::expected<TimeVal, RemoteIo::RemoteError> parseTimeVal(std::string_view text)
    {
        const auto invalid = [text]()
        {
            return std::unexpected(protocolViolation(fmt::format("Not a time-val: '{}'", printable(text))));
        };
        constexpr std::size_t wholeDigits = 14u;
        if (text.size() < wholeDigits)
            return invalid();
        for (std::size_t position = 0u; position < wholeDigits; ++position)
        {
            if (!isDigit(text[position]))
                return invalid();
        }

        std::chrono::microseconds fraction{};
        if (text.size() > wholeDigits)
        {
            const auto fractionDigits = text.substr(wholeDigits + 1u);
            if (text[wholeDigits] != '.' || fractionDigits.empty() || !std::ranges::all_of(fractionDigits, isDigit))
                return invalid();
            constexpr std::size_t microsecondDigits = 6u;
            for (std::size_t position = 0u; position < microsecondDigits; ++position)
            {
                const auto digit = position < fractionDigits.size() ? fractionDigits[position] - '0' : 0;
                fraction = fraction * 10 + std::chrono::microseconds{digit};
            }
        }

        const auto year = static_cast<int>(digitsAt(text, 0u, 4u));
        const std::chrono::year_month_day date{
            std::chrono::year{year},
            std::chrono::month{digitsAt(text, 4u, 2u)},
            std::chrono::day{digitsAt(text, 6u, 2u)},
        };
        const auto hour = digitsAt(text, 8u, 2u);
        const auto minute = digitsAt(text, 10u, 2u);
        const auto second = digitsAt(text, 12u, 2u);
        if (year < 1000 || !date.ok() || hour > 23u || minute > 59u || second > 60u)
            return invalid();

        return TimeVal{std::chrono::sys_days{date}} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
            std::chrono::seconds{second} + fraction;
    }

    std::optional<std::string> formatTimeVal(std::chrono::sys_seconds time)
    {
        const auto days = std::chrono::floor<std::chrono::days>(time);
        const std::chrono::year_month_day date{days};
        const auto year = static_cast<int>(date.year());
        if (year < 1000 || year > 9999)
            return std::nullopt;
        const std::chrono::hh_mm_ss clock{time - days};
        return fmt::format(
            "{:04}{:02}{:02}{:02}{:02}{:02}",
            year,
            static_cast<unsigned>(date.month()),
            static_cast<unsigned>(date.day()),
            clock.hours().count(),
            clock.minutes().count(),
            clock.seconds().count()
        );
    }

    std::expected<std::uint64_t, RemoteIo::RemoteError> parseSizeReply(Reply const& reply)
    {
        if (reply.code != 213)
            return std::unexpected(replyError(reply, Verb::Size));
        const auto text = Detail::trimmedSpaces(textOfLine(reply.lines.front()));
        std::size_t position = 0u;
        const auto size = numberAt<std::uint64_t>(text, position);
        if (!size || position != text.size())
            return std::unexpected(protocolViolation(fmt::format("Not a size: '{}'", printable(text)), reply.code));
        return *size;
    }

    std::expected<TimeVal, RemoteIo::RemoteError> parseModificationTimeReply(Reply const& reply)
    {
        if (reply.code != 213)
            return std::unexpected(replyError(reply, Verb::ModificationTime));
        return parseTimeVal(Detail::trimmedSpaces(textOfLine(reply.lines.front())))
            .transform_error(
                [&reply](RemoteIo::RemoteError error)
                {
                    error.protocolCode = reply.code;
                    return error;
                }
            );
    }

    std::expected<PassiveEndpoint, RemoteIo::RemoteError> parsePassiveReply(Reply const& reply)
    {
        if (reply.code != 227)
            return std::unexpected(replyError(reply, Verb::Passive));
        for (auto const& line : reply.lines)
        {
            const auto text = textOfLine(line);
            for (std::size_t position = 0u; position < text.size(); ++position)
            {
                const auto previous = position == 0u ? ' ' : text[position - 1u];
                if (!isDigit(text[position]) || isDigit(previous) || previous == ',' || previous == '-')
                    continue;
                auto end = position;
                if (auto endpoint = passiveEndpointAt(text, end, reply.code))
                    return std::move(*endpoint);
                position = end;
            }
        }
        return std::unexpected(protocolViolation("The passive reply names no host and port", reply.code));
    }

    std::expected<std::uint16_t, RemoteIo::RemoteError> parseExtendedPassiveReply(Reply const& reply)
    {
        if (reply.code != 229)
            return std::unexpected(replyError(reply, Verb::ExtendedPassive));
        for (auto const& line : reply.lines)
        {
            if (auto port = extendedPassivePortIn(textOfLine(line), reply.code))
                return std::move(*port);
        }
        return std::unexpected(protocolViolation("The extended passive reply names no port", reply.code));
    }

    std::expected<std::string, RemoteIo::RemoteError> parsePathReply(Reply const& reply)
    {
        if (reply.code != 257)
            return std::unexpected(replyError(reply, Verb::PrintWorkingDirectory));
        const auto text = textOfLine(reply.lines.front());
        const auto open = text.find('"');
        if (open == std::string_view::npos)
            return std::unexpected(protocolViolation("The path reply has no quoted path", reply.code));

        std::string path{};
        for (auto position = open + 1u; position < text.size(); ++position)
        {
            const auto character = text[position];
            const auto following = position + 1u < text.size() ? text[position + 1u] : ' ';
            if (character == '"' && following != '"')
            {
                if (path.empty())
                    return std::unexpected(protocolViolation("The path reply quotes an empty path", reply.code));
                return path;
            }
            path += character;
            if ((character == '"' && following == '"') || (character == '\r' && following == '\0'))
                ++position;
        }
        return std::unexpected(protocolViolation("The quoted path of the path reply does not end", reply.code));
    }
}
