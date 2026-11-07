#include <ftp/protocol/reply_error.hpp>
#include <ftp/protocol/protocol_error.hpp>
#include <ftp/protocol/reply_code.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

namespace FileTransferProtocol
{
    namespace
    {
        using RemoteIo::RemoteErrorKind;

        bool mentionsAny(std::string_view text, std::initializer_list<std::string_view> phrases)
        {
            return std::ranges::any_of(
                phrases,
                [text](std::string_view phrase)
                {
                    return text.find(phrase) != std::string_view::npos;
                }
            );
        }

        bool mentionsConnectionLimit(std::string_view text)
        {
            return mentionsAny(text, {"too many", "no more connections", "maximum number of"});
        }

        /**
         * @brief The kind an errno text names, as proftpd and pure-ftpd send them.
         */
        std::optional<RemoteErrorKind> kindNamedBy(std::string_view text)
        {
            if (mentionsAny(
                    text, {"no such file", "no such directory", "not found", "does not exist", "doesn't exist"}
                ))
                return RemoteErrorKind::NoSuchFile;
            if (mentionsAny(text, {"permission denied", "access denied", "access is denied", "not permitted"}))
                return RemoteErrorKind::PermissionDenied;
            if (mentionsAny(text, {"file exists", "already exists"}))
                return RemoteErrorKind::AlreadyExists;
            if (mentionsAny(text, {"no space left"}))
                return RemoteErrorKind::NoSpace;
            if (mentionsAny(text, {"quota"}))
                return RemoteErrorKind::QuotaExceeded;
            return std::nullopt;
        }

        RemoteErrorKind kindOf(int code, std::string_view text, std::optional<Verb> verb)
        {
            switch (code)
            {
                case 421:
                    if (mentionsConnectionLimit(text))
                        return RemoteErrorKind::ConnectionLimit;
                    return verb ? RemoteErrorKind::ConnectionLost : RemoteErrorKind::NoConnection;
                case 425:
                case 434:
                    return RemoteErrorKind::NoConnection;
                case 426:
                    return RemoteErrorKind::ConnectionLost;
                case 430:
                    return RemoteErrorKind::AuthenticationFailed;
                case 431:
                case 502:
                case 504:
                case 522:
                case 536:
                    return RemoteErrorKind::Unsupported;
                case 450:
                case 451:
                case 550:
                    return kindNamedBy(text).value_or(RemoteErrorKind::Failure);
                case 452:
                    return RemoteErrorKind::NoSpace;
                case 500:
                    // vsftpd reports its fatal errors as "500 OOPS: ...".
                    if (text.starts_with("oops"))
                        return RemoteErrorKind::Failure;
                    return verb ? RemoteErrorKind::Unsupported : RemoteErrorKind::NoConnection;
                case 503:
                    return RemoteErrorKind::ProtocolViolation;
                case 530:
                    if (mentionsConnectionLimit(text))
                        return RemoteErrorKind::ConnectionLimit;
                    return RemoteErrorKind::AuthenticationFailed;
                case 532:
                case 533:
                case 534:
                    return RemoteErrorKind::PermissionDenied;
                case 552:
                    return RemoteErrorKind::QuotaExceeded;
                case 553:
                    return kindNamedBy(text).value_or(RemoteErrorKind::PermissionDenied);
                default:
                    return RemoteErrorKind::Failure;
            }
        }
    }

    RemoteIo::RemoteError replyError(Reply const& reply, std::optional<Verb> verb)
    {
        const auto replyClass = classifyReply(reply.code);
        const auto answered = verb ? std::string{wireName(*verb)} : std::string{"the greeting"};
        const auto text = reply.text();
        if (replyClass != ReplyClass::TransientNegative && replyClass != ReplyClass::PermanentNegative)
            return protocolViolation(
                fmt::format("Unexpected reply {} to {}: {}", reply.code, answered, text), reply.code
            );

        return RemoteIo::RemoteError{
            .kind = kindOf(reply.code, Utility::Algorithm::toLowerCase(text), verb),
            .protocol = RemoteIo::Protocol::Ftp,
            .protocolCode = reply.code,
            .message = text,
        };
    }
}
