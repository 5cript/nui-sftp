#include <ftp/protocol/reply.hpp>
#include <ftp/protocol/protocol_error.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        // RFC 854.
        constexpr unsigned char interpretAsCommand = 255u;
        constexpr unsigned char will = 251u;
        constexpr unsigned char dont = 254u;

        /**
         * @brief @p line without the code and separator it starts with when that code is @p code.
         */
        std::string_view withoutCode(std::string_view line, int code)
        {
            if (replyCodeOf(line) != code)
                return line;
            return textOfLine(line);
        }

        bool endsWithSpaceOrLine(std::string_view line)
        {
            return line.size() == 3u || line[3] == ' ';
        }

        /**
         * @brief The start of a line that is no reply, for an error message.
         */
        std::string excerpt(std::string_view line)
        {
            constexpr std::size_t shown = 40u;
            if (line.size() <= shown)
                return printable(line);
            return fmt::format("{}...", printable(line.substr(0u, shown)));
        }
    }

    std::string Reply::text() const
    {
        const auto openingCode = lines.empty() ? std::nullopt : replyCodeOf(lines.front());
        std::string joined{};
        for (auto const& line : lines)
        {
            auto stripped = withoutCode(line, openingCode.value_or(code));
            if (stripped.size() == line.size())
                stripped = withoutCode(line, code);
            if (!joined.empty())
                joined += '\n';
            joined += printable(stripped);
        }
        return joined;
    }

    std::optional<int> replyCodeOf(std::string_view line)
    {
        if (line.size() < 3u || line[0] < '1' || line[0] > '5')
            return std::nullopt;
        if (line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9')
            return std::nullopt;
        if (line.size() > 3u && line[3] != ' ' && line[3] != '-')
            return std::nullopt;
        return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    }

    std::string_view textOfLine(std::string_view line)
    {
        if (!replyCodeOf(line))
            return line;
        return line.substr(std::min<std::size_t>(line.size(), 4u));
    }

    ReplyFramer::ReplyFramer(ReplyLimits limits)
        : limits_{limits}
    {}

    std::expected<void, RemoteIo::RemoteError> ReplyFramer::feed(std::string_view bytes)
    {
        if (failure_)
            return std::unexpected(*failure_);
        for (const auto character : bytes)
        {
            const auto taken = takeByte(static_cast<unsigned char>(character));
            if (!taken)
            {
                failure_ = taken.error();
                return taken;
            }
        }
        return {};
    }

    std::optional<Reply> ReplyFramer::next()
    {
        if (complete_.empty())
            return std::nullopt;
        auto reply = std::move(complete_.front());
        complete_.pop_front();
        return reply;
    }

    bool ReplyFramer::holdsPossibleEnd() const
    {
        return possibleEnd_.has_value();
    }

    void ReplyFramer::settle()
    {
        // Bytes of a following line show that the possible end was text.
        if (!possibleEnd_ || !line_.empty() || telnetState_ != TelnetState::Data)
            return;
        auto lastLine = std::move(*possibleEnd_);
        possibleEnd_.reset();
        const auto code = replyCodeOf(lastLine).value_or(0);
        open_->endedOnOtherCode = true;
        completeOpenReply(std::move(lastLine), code);
    }

    std::expected<void, RemoteIo::RemoteError> ReplyFramer::finish()
    {
        if (failure_)
            return std::unexpected(*failure_);
        telnetState_ = TelnetState::Data;
        if (!line_.empty())
        {
            const auto taken = takeLine(std::exchange(line_, {}));
            if (!taken)
            {
                failure_ = taken.error();
                return taken;
            }
        }
        settle();
        if (open_)
        {
            return std::unexpected(
                RemoteIo::RemoteError{
                    .kind = RemoteIo::RemoteErrorKind::ConnectionLost,
                    .protocol = RemoteIo::Protocol::Ftp,
                    .protocolCode = replyCodeOf(open_->lines.front()).value_or(0),
                    .message = "The server closed the connection in the middle of a reply",
                }
            );
        }
        return {};
    }

    bool ReplyFramer::inReply() const
    {
        return open_.has_value() || !line_.empty();
    }

    std::expected<void, RemoteIo::RemoteError> ReplyFramer::takeByte(unsigned char byte)
    {
        switch (telnetState_)
        {
            case TelnetState::Data:
                if (byte == interpretAsCommand)
                {
                    telnetState_ = TelnetState::Command;
                    return {};
                }
                break;
            case TelnetState::Command:
                telnetState_ = TelnetState::Data;
                if (byte >= will && byte <= dont)
                {
                    telnetState_ = TelnetState::Option;
                    return {};
                }
                // IAC IAC is one 0xFF of data. Before any other byte a server that does not escape 0xFF sent it as
                // data: the client agrees to no option, so no subnegotiation can start, and the other commands mean
                // nothing on a control connection.
                if (byte != interpretAsCommand)
                {
                    const auto kept = keepInLine(interpretAsCommand);
                    if (!kept)
                        return kept;
                }
                break;
            case TelnetState::Option:
                telnetState_ = TelnetState::Data;
                return {};
        }

        if (byte == '\n')
        {
            auto line = std::exchange(line_, {});
            if (line.ends_with('\r'))
                line.pop_back();
            return takeLine(std::move(line));
        }
        return keepInLine(byte);
    }

    std::expected<void, RemoteIo::RemoteError> ReplyFramer::keepInLine(unsigned char byte)
    {
        if (line_.size() + 1u >= limits_.lineLength)
            return std::unexpected(
                protocolViolation(fmt::format("A line is longer than {} bytes", limits_.lineLength))
            );
        line_ += static_cast<char>(byte);
        return {};
    }

    std::expected<void, RemoteIo::RemoteError> ReplyFramer::takeLine(std::string line)
    {
        if (!open_)
        {
            if (line.empty())
                return {};
            const auto code = replyCodeOf(line);
            if (!code)
                return std::unexpected(protocolViolation(fmt::format("Not a reply: '{}'", excerpt(line))));
            if (line.size() > 3u && line[3] == '-')
            {
                openSize_ = line.size();
                open_ = Reply{.code = *code, .lines = {std::move(line)}};
                return {};
            }
            complete_.push_back(Reply{.code = *code, .lines = {std::move(line)}});
            return {};
        }

        // The next line decides: a possible end followed by anything was text.
        if (possibleEnd_)
        {
            keepInOpenReply(std::move(*possibleEnd_));
            possibleEnd_.reset();
        }

        const auto openingCode = replyCodeOf(open_->lines.front()).value_or(0);
        const auto code = replyCodeOf(line);
        if (code && endsWithSpaceOrLine(line))
        {
            if (*code == openingCode)
                completeOpenReply(std::move(line), *code);
            else
                possibleEnd_ = std::move(line);
            return {};
        }
        keepInOpenReply(std::move(line));
        return {};
    }

    void ReplyFramer::keepInOpenReply(std::string line)
    {
        if (open_->lines.size() >= limits_.lineCount || openSize_ + line.size() > limits_.replySize)
        {
            ++open_->droppedLines;
            return;
        }
        openSize_ += line.size();
        open_->lines.push_back(std::move(line));
    }

    void ReplyFramer::completeOpenReply(std::string lastLine, int code)
    {
        auto reply = std::move(*open_);
        open_.reset();
        openSize_ = 0u;
        reply.code = code;
        reply.lines.push_back(std::move(lastLine));
        complete_.push_back(std::move(reply));
    }
}
