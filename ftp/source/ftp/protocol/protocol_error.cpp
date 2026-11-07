#include <ftp/protocol/protocol_error.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>

#include <utility>

namespace FileTransferProtocol
{
    RemoteIo::RemoteError protocolViolation(std::string message, int replyCode)
    {
        return RemoteIo::RemoteError{
            .kind = RemoteIo::RemoteErrorKind::ProtocolViolation,
            .protocol = RemoteIo::Protocol::Ftp,
            .protocolCode = replyCode,
            .message = std::move(message),
        };
    }

    std::string printable(std::string_view bytes)
    {
        std::string text{};
        text.reserve(bytes.size());
        std::size_t position = 0u;
        while (position < bytes.size())
        {
            const auto byte = static_cast<unsigned char>(bytes[position]);
            const auto length = Paths::Detail::utf8SequenceLength(bytes, position);
            if (length == 0u || byte < 0x20u || byte == 0x7Fu)
            {
                text += fmt::format("\\x{:02X}", byte);
                ++position;
                continue;
            }
            text.append(bytes.substr(position, length));
            position += length;
        }
        return text;
    }
}
