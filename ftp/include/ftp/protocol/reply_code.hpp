#pragma once

#include <optional>

namespace FileTransferProtocol
{
    /**
     * @brief What the first digit of a reply code says, RFC 959 4.2.
     */
    enum class ReplyClass
    {
        PositivePreliminary,
        PositiveCompletion,
        PositiveIntermediate,
        TransientNegative,
        PermanentNegative,
    };

    /**
     * @brief Classifies a reply by its first digit alone, so codes the client does not know work too.
     *
     * @param code The three digit code of the reply.
     * @return The class, nothing when @p code is not between 100 and 599.
     */
    std::optional<ReplyClass> classifyReply(int code);

    /**
     * @brief A 1yz reply, which is never the final reply to a command.
     */
    bool isPreliminary(int code);
}
