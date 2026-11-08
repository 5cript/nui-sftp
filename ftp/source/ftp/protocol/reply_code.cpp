#include <ftp/protocol/reply_code.hpp>

namespace FileTransferProtocol
{
    std::optional<ReplyClass> classifyReply(int code)
    {
        switch (code / 100)
        {
            case 1:
                return ReplyClass::PositivePreliminary;
            case 2:
                return ReplyClass::PositiveCompletion;
            case 3:
                return ReplyClass::PositiveIntermediate;
            case 4:
                return ReplyClass::TransientNegative;
            case 5:
                return ReplyClass::PermanentNegative;
            default:
                return std::nullopt;
        }
    }

    bool isPreliminary(int code)
    {
        return classifyReply(code) == ReplyClass::PositivePreliminary;
    }
}
