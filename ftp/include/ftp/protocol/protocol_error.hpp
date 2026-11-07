#pragma once

#include <remote_io/remote_error.hpp>

#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    /**
     * @brief The server broke the protocol, the connection cannot be trusted any more.
     *
     * @param message What was wrong, already printable.
     * @param replyCode The code of the reply that broke it, 0 for none.
     */
    RemoteIo::RemoteError protocolViolation(std::string message, int replyCode = 0);

    /**
     * @brief @p bytes as text a log or a person can read: valid UTF-8 stays, every other byte and the bytes of every
     *        control character become \\xNN. Control characters are C0, DEL and C1, the bidirectional formatting
     *        characters and the line and paragraph separators.
     */
    std::string printable(std::string_view bytes);
}
