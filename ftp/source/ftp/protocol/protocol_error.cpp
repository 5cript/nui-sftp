#include <ftp/protocol/protocol_error.hpp>
#include <paths/utf8.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace FileTransferProtocol
{
    namespace
    {
        /**
         * @brief C0, DEL and C1 controls, and the characters that change the direction or the lines of text around
         *        them.
         */
        bool isControl(char32_t codePoint)
        {
            return codePoint < 0x20u || (codePoint >= 0x7Fu && codePoint <= 0x9Fu) || codePoint == 0x061Cu ||
                codePoint == 0x200Eu || codePoint == 0x200Fu || (codePoint >= 0x2028u && codePoint <= 0x202Eu) ||
                (codePoint >= 0x2066u && codePoint <= 0x2069u);
        }
    }

    RemoteIo::RemoteError protocolViolation(std::string message, int replyCode)
    {
        return RemoteIo::RemoteError{
            .kind = RemoteIo::RemoteErrorKind::ProtocolViolation,
            .protocol = RemoteIo::Protocol::Ftp,
            .protocolCode = replyCode,
            .message = std::move(message),
        };
    }

    RemoteIo::RemoteError unreadableReply(std::string message, int replyCode)
    {
        return RemoteIo::RemoteError{
            .kind = RemoteIo::RemoteErrorKind::Failure,
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
            const auto length = Paths::Detail::utf8SequenceLength(bytes, position);
            const auto taken = std::max<std::size_t>(length, 1u);
            if (length != 0u && !isControl(Paths::Detail::utf8CodePointAt(bytes, position, length)))
                text.append(bytes.substr(position, taken));
            else
            {
                for (const auto byte : bytes.substr(position, taken))
                    text += fmt::format("\\x{:02X}", static_cast<unsigned char>(byte));
            }
            position += taken;
        }
        return text;
    }
}
